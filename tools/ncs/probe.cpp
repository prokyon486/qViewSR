// SPDX-License-Identifier: GPL-3.0-or-later
// OpenVINO 2020.3 diagnostic. No Qt/OpenCV dependency and no implicit CPU fallback.
#include <inference_engine.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::string json(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c < 32) {
            const char* hex = "0123456789abcdef";
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += c;
    }
    return out + "\"";
}

int main(int argc, char** argv) {
    std::string model, device;
    bool infer = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--model" && i + 1 < argc) model = argv[++i];
            else if (arg == "--device" && i + 1 < argc) device = argv[++i];
            else if (arg == "--infer") infer = true;
            else throw std::runtime_error("Usage: ncs-probe [--model 1032.xml --device ID --infer]");
        }
        if (infer && (model.empty() || device.empty()))
            throw std::runtime_error("Inference requires an explicit model and device ID (or CPU).");

        InferenceEngine::Core core;
        const auto* version = InferenceEngine::GetInferenceEngineVersion();
        std::cout << "{\"event\":\"runtime\",\"build\":" << json(version->buildNumber) << "}" << std::endl;
        // CPU smoke test must remain possible even if the MYRIAD plugin cannot load.
        std::vector<std::string> ids;
        if (device != "CPU") {
            ids = core.GetMetric("MYRIAD", METRIC_KEY(AVAILABLE_DEVICES)).as<std::vector<std::string>>();
            for (const auto& id : ids) {
                std::cout << "{\"event\":\"device\",\"id\":" << json(id)
                          << ",\"inference_verified\":false}" << std::endl;
            }
            if (ids.empty()) {
                std::cout << "{\"event\":\"unavailable\",\"reason\":\"No MYRIAD devices enumerated; check USB visibility and permissions.\"}" << std::endl;
                return 2;
            }
        }
        if (!infer) return 0;
        if (device != "CPU" && std::find(ids.begin(), ids.end(), device) == ids.end())
            throw std::runtime_error("Requested device ID was not enumerated in this session.");

        auto network = core.ReadNetwork(model);
        auto inputs = network.getInputsInfo();
        auto outputs = network.getOutputsInfo();
        const InferenceEngine::SizeVector lr{1, 3, 270, 480}, hr{1, 3, 1080, 1920};
        if (inputs.size() != 2 || !inputs.count("0") || !inputs.count("1") ||
            inputs.at("0")->getTensorDesc().getDims() != lr ||
            inputs.at("1")->getTensorDesc().getDims() != hr || outputs.size() != 1 ||
            outputs.begin()->second->getTensorDesc().getDims() != hr)
            throw std::runtime_error("Model does not match the pinned 1032 tensor contract.");
        for (auto& input : inputs) input.second->setPrecision(InferenceEngine::Precision::FP32);
        for (auto& output : outputs) output.second->setPrecision(InferenceEngine::Precision::FP32);
        const auto before = std::chrono::steady_clock::now();
        auto executable = device == "CPU" ? core.LoadNetwork(network, "CPU") :
            core.LoadNetwork(network, "MYRIAD", {{CONFIG_KEY(DEVICE_ID), device}});
        auto request = executable.CreateInferRequest();
        // Constant grey: bicubic of a constant is the same constant, so no OpenCV is needed.
        for (const auto& input : inputs) {
            auto blob = request.GetBlob(input.first);
            std::fill_n(blob->buffer().as<float*>(), blob->size(), 128.0f);
        }
        const auto loaded = std::chrono::steady_clock::now();
        request.Infer();
        const auto finished = std::chrono::steady_clock::now();
        auto output = request.GetBlob(outputs.begin()->first);
        auto* data = output->buffer().as<float*>();
        double sum = 0;
        float low = data[0], high = data[0];
        for (size_t i = 0; i < output->size(); ++i) {
            if (!std::isfinite(data[i])) throw std::runtime_error("Non-finite model output.");
            sum += data[i]; low = std::min(low, data[i]); high = std::max(high, data[i]);
        }
        const double mean = sum / output->size();
        if (mean < 0.1 || mean > 0.9) throw std::runtime_error("Grey smoke test output has implausible mean.");
        std::cout << std::setprecision(8) << "{\"event\":\"smoke_pass\",\"device\":" << json(device)
                  << ",\"inference_verified\":true,\"load_ms\":"
                  << std::chrono::duration<double, std::milli>(loaded - before).count()
                  << ",\"infer_ms\":" << std::chrono::duration<double, std::milli>(finished - loaded).count()
                  << ",\"min\":" << low << ",\"max\":" << high << ",\"mean\":" << mean
                  << ",\"note\":\"Constant-input smoke test only; not image quality or throughput validation.\"}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"event\":\"error\",\"message\":" << json(error.what()) << "}" << std::endl;
        return 1;
    }
}
