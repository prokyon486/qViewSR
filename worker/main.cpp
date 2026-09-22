// SPDX-License-Identifier: GPL-3.0-or-later
// Tensor preprocessing follows Open Model Zoo 2020.3 super_resolution_demo.
#include "tile_plan.h"
#include <inference_engine.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

class Events {
    FILE* stream;
    std::mutex mutex;
public:
    std::string jobId;
    Events() {
        const int output = dup(STDOUT_FILENO);
        if (output < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
            throw std::runtime_error("Cannot isolate worker event channel.");
        stream = fdopen(output, "w");
        if (!stream) throw std::runtime_error("Cannot open worker event channel.");
    }
    ~Events() { fclose(stream); }
    void send(Json value) {
        value["protocol_version"] = 1;
        value["job_id"] = jobId;
        const auto line = value.dump() + "\n";
        if (line.size() > 65536) throw std::runtime_error("Worker event too large.");
        std::lock_guard<std::mutex> guard(mutex);
        if (fwrite(line.data(), 1, line.size(), stream) != line.size() || fflush(stream))
            throw std::runtime_error("Worker event pipe closed.");
    }
};

static std::string hashFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read file: " + path);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot initialize SHA-256.");
    char buffer[65536];
    while (file.read(buffer, sizeof buffer) || file.gcount())
        if (EVP_DigestUpdate(context.get(), buffer, size_t(file.gcount())) != 1)
            throw std::runtime_error("SHA-256 update failed.");
    if (!file.eof()) throw std::runtime_error("File read failed: " + path);
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &length) != 1) throw std::runtime_error("SHA-256 failed.");
    std::ostringstream out;
    for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setfill('0') << std::setw(2) << int(digest[i]);
    return out.str();
}

class DeviceLock {
    int fd = -1;
public:
    explicit DeviceLock(const std::string& path) {
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) throw std::runtime_error("Cannot open device ownership lock.");
        if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); fd = -1; throw std::runtime_error("Another qViewSR worker is using the devices."); }
    }
    ~DeviceLock() { if (fd >= 0) close(fd); }
};

static void checkPng(const std::string& path, int width, int height) {
    unsigned char h[24]{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(h), sizeof h)) throw std::runtime_error("Truncated input PNG.");
    const unsigned char signature[]{137,80,78,71,13,10,26,10};
    auto number = [&](int offset) { return uint32_t(h[offset]) << 24 | uint32_t(h[offset+1]) << 16 | uint32_t(h[offset+2]) << 8 | h[offset+3]; };
    if (!std::equal(h, h+8, signature) || std::string(reinterpret_cast<char*>(h+12), 4) != "IHDR" ||
        number(16) != uint32_t(width) || number(20) != uint32_t(height))
        throw std::runtime_error("Input PNG dimensions differ from job.");
}

struct Device {
    std::string id;
    InferenceEngine::ExecutableNetwork executable;
    InferenceEngine::InferRequest request;
    int tiles = 0;
    double inferMs = 0;
};

static void fillBlob(const cv::Mat& image, InferenceEngine::Blob::Ptr blob) {
    float* data = blob->buffer().as<float*>();
    const size_t plane = size_t(image.rows) * image.cols;
    for (int y = 0; y < image.rows; ++y) {
        const auto* row = image.ptr<cv::Vec3b>(y);
        for (int x = 0; x < image.cols; ++x)
            for (int c = 0; c < 3; ++c) data[c * plane + size_t(y) * image.cols + x] = row[x][c];
    }
}

static void processTile(Device& device, const cv::Mat& source, cv::Mat& result, const sr::Tile& tile, int halo,
                        const std::string& outputName) {
    cv::Mat input(sr::tileHeight, sr::tileWidth, CV_8UC3);
    for (int y = 0; y < input.rows; ++y) {
        const auto* sourceRow = source.ptr<cv::Vec3b>(sr::reflect(tile.y - halo + y, source.rows));
        auto* row = input.ptr<cv::Vec3b>(y);
        for (int x = 0; x < input.cols; ++x) row[x] = sourceRow[sr::reflect(tile.x - halo + x, source.cols)];
    }
    cv::Mat bicubic;
    cv::resize(input, bicubic, cv::Size(sr::tileWidth*4, sr::tileHeight*4), 0, 0, cv::INTER_CUBIC);
    fillBlob(input, device.request.GetBlob("0"));
    fillBlob(bicubic, device.request.GetBlob("1"));
    const auto started = Clock::now();
    device.request.Infer();
    device.inferMs += ms(started);
    const auto blob = device.request.GetBlob(outputName);
    const float* pixels = blob->buffer().as<float*>();
    const size_t plane = size_t(sr::tileWidth*4) * sr::tileHeight*4;
    for (int y = 0; y < tile.height*4; ++y) {
        auto* row = result.ptr<cv::Vec3b>(tile.y*4+y);
        for (int x = 0; x < tile.width*4; ++x) {
            const size_t offset = size_t(y+halo*4) * sr::tileWidth*4 + x+halo*4;
            for (int c = 0; c < 3; ++c) {
                const float value = pixels[c * plane + offset];
                if (!std::isfinite(value)) throw std::runtime_error("Non-finite inference output.");
                row[tile.x*4+x][c] = cv::saturate_cast<unsigned char>(value * 255.0f);
            }
        }
    }
}

static void run(const Json& job, Events& events) {
    if (job.at("protocol_version") != 1) throw std::runtime_error("Unsupported protocol version.");
    events.jobId = job.at("job_id").get<std::string>();
    const auto started = Clock::now();
    const int width = job.at("width"), height = job.at("height"), halo = job.value("halo",16);
    const auto tiles = sr::plan(width, height, halo);
    const std::string inputPath = job.at("input"), outputPath = job.at("output"), model = job.at("model_xml");
    const std::string sourceKey = job.at("source_key");
    const auto budget = job.value("max_memory_bytes", uint64_t(2147483648ULL));
    if (uint64_t(width)*height*64 + 4*192ULL*1024*1024 > budget)
        throw std::runtime_error("Image exceeds worker memory budget.");
    checkPng(inputPath, width, height);
    if (hashFile(inputPath) != job.at("input_sha256")) throw std::runtime_error("Input checksum mismatch.");
    if (model.size() < 4 || model.substr(model.size()-4) != ".xml") throw std::runtime_error("Expected an IR XML model.");
    const auto bin = model.substr(0,model.size()-4) + ".bin";
    // These hashes identify the verified OMZ 2020.3 model, not merely its file name.
    if (hashFile(model) != "25be6418ee76c33169470ada73b924864fb439c5fc5a208b7fa4b0e64f6f84d1" ||
        hashFile(bin) != "abae5907d40ef7e47d680435a99484b74076a985a6b8c3353b64fa77e6d3c149")
        throw std::runtime_error("Model checksum differs from pinned OMZ 2020.3 1032 FP16.");
    if (access(outputPath.c_str(), F_OK) == 0) throw std::runtime_error("Output file already exists.");
    DeviceLock ownership(job.at("lock_file"));
    events.send({{"event","started"},{"total",tiles.size()}});
    cv::setNumThreads(1);
    const cv::Mat source = cv::imread(inputPath, cv::IMREAD_COLOR);
    if (source.empty() || source.cols != width || source.rows != height) throw std::runtime_error("Input decode failed.");
    cv::Mat output(height*4, width*4, CV_8UC3);
    InferenceEngine::Core core;
    const std::string selection = job.value("devices",std::string("all"));
    std::vector<std::string> selected;
    if (selection == "CPU") selected.push_back("CPU");
    else {
        const auto available = core.GetMetric("MYRIAD",METRIC_KEY(AVAILABLE_DEVICES)).as<std::vector<std::string>>();
        std::set<std::string> explicitIds;
        if (selection != "all" && selection != "ncs2" && selection != "ncs") {
            std::istringstream stream(selection); std::string id;
            while (std::getline(stream,id,',')) explicitIds.insert(id);
        }
        for (const auto& id : available) {
            if (selection == "all" || (selection == "ncs2" && id.find("ma2480") != std::string::npos) ||
                (selection == "ncs" && id.find("ma2450") != std::string::npos) || explicitIds.count(id)) selected.push_back(id);
        }
        for (const auto& id : explicitIds)
            if (std::find(selected.begin(),selected.end(),id) == selected.end()) throw std::runtime_error("Selected device unavailable: " + id);
    }
    if (selected.empty()) throw std::runtime_error("No selected MYRIAD devices. Check the USB connection and permissions.");
    if (selected.size() > 4) selected.resize(4);
    auto network = core.ReadNetwork(model);
    auto inputs = network.getInputsInfo(); auto outputs = network.getOutputsInfo();
    const InferenceEngine::SizeVector lr{1,3,270,480}, hr{1,3,1080,1920};
    if (inputs.size()!=2 || !inputs.count("0") || !inputs.count("1") || outputs.size()!=1 ||
        inputs.at("0")->getTensorDesc().getDims()!=lr || inputs.at("1")->getTensorDesc().getDims()!=hr ||
        outputs.begin()->second->getTensorDesc().getDims()!=hr) throw std::runtime_error("Unexpected model tensor contract.");
    for (auto& input : inputs) input.second->setPrecision(InferenceEngine::Precision::FP32);
    outputs.begin()->second->setPrecision(InferenceEngine::Precision::FP32);
    const auto outputName = outputs.begin()->first;
    std::vector<std::unique_ptr<Device>> devices;
    for (const auto& id : selected) {
        try {
            std::unique_ptr<Device> device(new Device);
            device->id=id;
            device->executable = id == "CPU" ? core.LoadNetwork(network,"CPU",{{"CPU_THREADS_NUM","8"}}) :
                core.LoadNetwork(network,"MYRIAD",{{CONFIG_KEY(DEVICE_ID),id}});
            device->request=device->executable.CreateInferRequest();
            devices.push_back(std::move(device));
            events.send({{"event","device_ready"},{"device",id}});
        } catch (const std::exception& e) { events.send({{"event","device_error"},{"device",id},{"message",e.what()}}); }
    }
    if (devices.empty()) throw std::runtime_error("No device could load the model. Original image is retained.");
    struct Work { size_t index; int attempt; };
    std::deque<Work> queue;
    for (size_t i=0;i<tiles.size();++i) queue.push_back({i,0});
    std::mutex mutex;
    std::condition_variable changed;
    size_t completed=0, running=0;
    std::string failure;
    std::vector<std::thread> threads;
    // Separate executable/request and input buffers per device. Output rectangles do not overlap.
    auto consume = [&](Device& device) {
        for (;;) {
            Work work;
            {
                std::unique_lock<std::mutex> guard(mutex);
                changed.wait(guard,[&]{return !failure.empty() || !queue.empty() || running==0;});
                if (!failure.empty() || queue.empty()) return;
                work=queue.front(); queue.pop_front(); ++running;
            }
            try {
                processTile(device,source,output,tiles[work.index],halo,outputName);
                std::lock_guard<std::mutex> guard(mutex);
                --running; ++completed; ++device.tiles;
                events.send({{"event","progress"},{"completed",completed},{"total",tiles.size()},{"device",device.id}});
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> guard(mutex);
                --running;
                if (work.attempt==0) queue.push_back({work.index,1});
                else failure=e.what();
                events.send({{"event","device_error"},{"device",device.id},{"message",e.what()}});
                changed.notify_all();
                return; // quarantine; another ready device may retry this tile once
            }
            changed.notify_all();
        }
    };
    try {
        for (auto& device : devices) threads.emplace_back(consume,std::ref(*device));
    } catch (...) {
        { std::lock_guard<std::mutex> guard(mutex); failure="Cannot create device thread."; }
        changed.notify_all();
        for (auto& thread : threads) thread.join();
        throw;
    }
    for (auto& thread : threads) thread.join();
    if (completed!=tiles.size()) throw std::runtime_error("Incomplete SR result: " + (failure.empty()?std::string("all devices failed"):failure));
    const auto temporary = outputPath + ".part.png";
    try {
        if (!cv::imwrite(temporary,output)) throw std::runtime_error("Cannot encode output PNG.");
        if (std::rename(temporary.c_str(),outputPath.c_str())) throw std::runtime_error("Cannot publish output PNG.");
    } catch (...) { std::remove(temporary.c_str()); throw; }
    Json stats=Json::array();
    for (const auto& device : devices) stats.push_back({{"id",device->id},{"tiles",device->tiles},{"infer_ms",device->inferMs}});
    events.send({{"event","completed"},{"source_key",sourceKey},{"width",output.cols},{"height",output.rows},
        {"color_space","sRGB"},{"output_sha256",hashFile(outputPath)},{"wall_ms",ms(started)},{"devices",stats}});
}

int main(int argc,char** argv) {
    Events events;
    try {
        if (argc==2 && std::string(argv[1])=="--list") {
            InferenceEngine::Core core;
            const auto ids=core.GetMetric("MYRIAD",METRIC_KEY(AVAILABLE_DEVICES)).as<std::vector<std::string>>();
            events.send({{"event","devices"},{"devices",ids}}); return 0;
        }
        if (argc!=3 || std::string(argv[1])!="--job") throw std::runtime_error("Usage: ncs-sr-worker --job job.json | --list");
        struct stat st{};
        if (stat(argv[2],&st) || st.st_size>65536) throw std::runtime_error("Invalid job file.");
        std::ifstream file(argv[2]); Json job; file >> job;
        run(job,events); return 0;
    } catch (const std::exception& e) {
        events.send({{"event","error"},{"message",e.what()}}); return 1;
    }
}
