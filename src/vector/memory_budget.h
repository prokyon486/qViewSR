// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSize>
#include <QString>
#include <QtGlobal>

namespace Vector {

// Remaining Linux host/cgroup memory, taking the tightest known limit.
// Zero means unavailable information. A known exhausted limit returns one byte
// so callers do not mistake memory pressure for an unsupported platform.
quint64 availableMemoryBytes();

// Empty means no preflight rejection. This is an estimate, not a reservation;
// allocation failures and cancellation still need handling at the call site.
QString preparationMemoryError(QSize sourceSize);

// Bound serialized SVGs independently of input dimensions. The estimate covers
// parsing, imported DOM nodes and rendering; it is not a memory reservation.
constexpr quint64 MaxSvgSerializedBytes = 1024ULL * 1024 * 1024;
QString svgMemoryError(quint64 serializedBytes, quint64 availableBytes);
QString svgMemoryError(quint64 serializedBytes);

}
