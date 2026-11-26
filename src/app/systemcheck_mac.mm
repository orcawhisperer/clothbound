#include "app/systemcheck.h"

#include <QByteArray>
#include <QString>
#include <QSysInfo>
#include <QThread>
#include <cstdint>
#include <mach/mach.h>
#include <sys/sysctl.h>

#undef slots
#undef signals
#undef emit

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

// Compiled with ARC (-fobjc-arc) and linked against Metal.framework and Foundation.framework.
#if !__has_feature(objc_arc)
#error "systemcheck_mac.mm must be compiled with ARC (-fobjc-arc)"
#endif

namespace {

QString sysctlString(const char *name) {
    size_t size = 0;
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return {};
    QByteArray buf(qsizetype(size), '\0');
    if (::sysctlbyname(name, buf.data(), &size, nullptr, 0) != 0)
        return {};
    while (!buf.isEmpty() && buf.endsWith('\0'))
        buf.chop(1);
    return QString::fromUtf8(buf).trimmed();
}

bool sysctlUint64(const char *name, uint64_t *out) {
    uint64_t val = 0;
    size_t size = sizeof(val);
    if (::sysctlbyname(name, &val, &size, nullptr, 0) != 0 || size != sizeof(val))
        return false;
    *out = val;
    return true;
}

bool sysctlInt32(const char *name, int32_t *out) {
    int32_t val = 0;
    size_t size = sizeof(val);
    if (::sysctlbyname(name, &val, &size, nullptr, 0) != 0 || size != sizeof(val))
        return false;
    *out = val;
    return true;
}

bool sysctlFlagEnabled(const char *name) {
    int32_t val = 0;
    return sysctlInt32(name, &val) && val != 0;
}

double queryAvailableMemoryGb(double totalGb) {
    mach_port_t host = mach_host_self();
    vm_size_t pageSize = 0;
    double availGb = 0.0;
    if (host_page_size(host, &pageSize) == KERN_SUCCESS && pageSize > 0) {
        vm_statistics64_data_t vmStat{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vmStat), &count) == KERN_SUCCESS) {
            const uint64_t availPages =
                uint64_t(vmStat.free_count) + uint64_t(vmStat.inactive_count) +
                uint64_t(vmStat.speculative_count) + uint64_t(vmStat.purgeable_count);
            availGb = double(availPages) * double(pageSize) / (1024.0 * 1024.0 * 1024.0);
        }
    }
    mach_port_deallocate(mach_task_self(), host);
    return totalGb > 0.0 ? qMin(availGb, totalGb) : availGb;
}

}  // namespace

SystemCheck::Machine SystemCheck::inspectMacHardware() {
    Machine machine;

    uint64_t memBytes = 0;
    if (sysctlUint64("hw.memsize", &memBytes) && memBytes > 0)
        machine.memoryGb = double(memBytes) / (1024.0 * 1024.0 * 1024.0);
    machine.availableGb = queryAvailableMemoryGb(machine.memoryGb);

    int32_t logicalCores = 0;
    if (sysctlInt32("hw.logicalcpu", &logicalCores) && logicalCores > 0)
        machine.cores = int(logicalCores);
    else
        machine.cores = qMax(1, QThread::idealThreadCount());

    machine.architecture = QSysInfo::currentCpuArchitecture();

    QString brand = sysctlString("machdep.cpu.brand_string");
    if (brand.isEmpty())
        brand = sysctlString("hw.model");
    machine.processor = brand;

    // CPU integer / matrix acceleration features for local LLMs.
    if (sysctlFlagEnabled("hw.optional.arm.FEAT_DotProd"))
        machine.features.append(QStringLiteral("dot product"));
    if (sysctlFlagEnabled("hw.optional.arm.FEAT_I8MM"))
        machine.features.append(QStringLiteral("i8mm"));
    if (sysctlFlagEnabled("hw.optional.arm.FEAT_BF16"))
        machine.features.append(QStringLiteral("BF16"));
    if (sysctlFlagEnabled("hw.optional.avx2_0"))
        machine.features.append(QStringLiteral("AVX2"));
    if (sysctlFlagEnabled("hw.optional.avx512vnni"))
        machine.features.append(QStringLiteral("AVX-512 VNNI"));

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device) {
            if (device.name)
                machine.graphics = QString::fromNSString(device.name);
            const bool unified = device.hasUnifiedMemory ? true : false;
            machine.discreteGraphics = !unified;
            const uint64_t workingSetBytes = uint64_t(device.recommendedMaxWorkingSetSize);
            if (workingSetBytes > 0)
                machine.graphicsMemoryGb = double(workingSetBytes) / (1024.0 * 1024.0 * 1024.0);
            if (!machine.features.contains(QStringLiteral("Metal")))
                machine.features.append(QStringLiteral("Metal"));
        }
    }

    if (machine.graphics.isEmpty() && machine.architecture == QLatin1String("arm64"))
        machine.graphics = QStringLiteral("Apple");
    if (machine.processor.isEmpty() &&
        (machine.graphics == QLatin1String("Apple") ||
         machine.graphics.startsWith(QLatin1String("Apple "), Qt::CaseInsensitive))) {
        machine.processor = QStringLiteral("Apple Silicon");
    }

    return machine;
}
