/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 * Copyright 2018 MoneroOcean      <https://github.com/MoneroOcean>, <support@moneroocean.stream>
 * Copyright 2018-2019 SChernykh   <https://github.com/SChernykh>
 * Copyright 2016-2019 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 * Copyright 2018-2019 MoneroOcean <https://github.com/MoneroOcean>, <support@moneroocean.stream>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <uv.h>
#include <inttypes.h>


#include "common/config/ConfigLoader.h"
#include "common/cpu/Cpu.h"
#include "core/Config.h"
#include "core/ConfigCreator.h"
#include "crypto/Asm.h"
#include "crypto/CryptoNight_constants.h"
#include "rapidjson/document.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "workers/CpuThread.h"

// for usage in Client::login to get_algo_perf
namespace xmrig {
    Config* pconfig = nullptr;
};

static char affinity_tmp[20] = { 0 };


xmrig::Config::Config() : xmrig::CommonConfig(),
    m_aesMode(AES_AUTO),
    m_algoVariant(AV_AUTO),
    m_assembly(ASM_AUTO),
    m_hugePages(true),
    m_safe(false),
    m_shouldSave(false),
    m_maxCpuUsage(100),
    m_priority(-1)
{
    // not defined algo performance is considered to be 0
    for (int a = 0; a != xmrig::PerfAlgo::PA_MAX; ++ a) {
        const xmrig::PerfAlgo pa = static_cast<xmrig::PerfAlgo>(a);
        m_algo_perf[pa] = 0.0f;
    }
}


bool xmrig::Config::reload(const char *json)
{
    return xmrig::ConfigLoader::reload(this, json);
}


void xmrig::Config::getJSON(rapidjson::Document &doc) const
{
    using namespace rapidjson;

    doc.SetObject();

    auto &allocator = doc.GetAllocator();

    doc.AddMember("algo", StringRef(algorithm().name()), allocator);

    Value api(kObjectType);
    api.AddMember("port",         apiPort(), allocator);
    api.AddMember("access-token", apiToken() ? Value(StringRef(apiToken())).Move() : Value(kNullType).Move(), allocator);
    api.AddMember("id",           apiId() ? Value(StringRef(apiId())).Move() : Value(kNullType).Move(), allocator);
    api.AddMember("worker-id",    apiWorkerId() ? Value(StringRef(apiWorkerId())).Move() : Value(kNullType).Move(), allocator);
    api.AddMember("ipv6",         isApiIPv6(), allocator);
    api.AddMember("restricted",   isApiRestricted(), allocator);
    doc.AddMember("api",          api, allocator);

#   ifndef XMRIG_NO_ASM
    doc.AddMember("asm",          Asm::toJSON(m_assembly), allocator);
#   endif

    doc.AddMember("autosave",     isAutoSave(), allocator);
    doc.AddMember("av",           algoVariant(), allocator);
    doc.AddMember("background",   isBackground(), allocator);
    doc.AddMember("colors",       isColors(), allocator);

    if (affinity() != -1L) {
        snprintf(affinity_tmp, sizeof(affinity_tmp) - 1, "0x%" PRIX64, affinity());
        doc.AddMember("cpu-affinity", StringRef(affinity_tmp), allocator);
    }
    else {
        doc.AddMember("cpu-affinity", kNullType, allocator);
    }

    doc.AddMember("cpu-priority",  priority() != -1 ? Value(priority()) : Value(kNullType), allocator);
    doc.AddMember("donate-level",  donateLevel(), allocator);
    doc.AddMember("huge-pages",    isHugePages(), allocator);
    doc.AddMember("hw-aes",        m_aesMode == AES_AUTO ? Value(kNullType) : Value(m_aesMode == AES_HW), allocator);
    doc.AddMember("log-file",      logFile()             ? Value(StringRef(logFile())).Move() : Value(kNullType).Move(), allocator);
    doc.AddMember("max-cpu-usage", m_maxCpuUsage, allocator);
    doc.AddMember("pools",         m_pools.toJSON(doc), allocator);
    doc.AddMember("print-time",    printTime(), allocator);
    doc.AddMember("retries",       m_pools.retries(), allocator);
    doc.AddMember("retry-pause",   m_pools.retryPause(), allocator);
    doc.AddMember("safe",          m_safe, allocator);

    // save extended "threads" based on m_threads
    Value threads(kObjectType);
    for (int a = 0; a != xmrig::Algo::ALGO_MAX; ++ a) {
        const xmrig::Algo algo = static_cast<xmrig::Algo>(a);
        Value key(xmrig::Algorithm::perfAlgoName(xmrig::Algorithm(algo).perf_algo()), allocator);
        if (threadsMode(algo) != Simple) {
            Value threads2(kArrayType);
            for (const IThread *thread : m_threads[algo].list) {
                threads2.PushBack(thread->toConfig(doc), allocator);
            }

            threads.AddMember(key, threads2, allocator);
        }
        else {
            threads.AddMember(key, threadsCount(), allocator);
        }
    }
    doc.AddMember("threads", threads, allocator);

    // save "algo-perf" based on m_algo_perf
    Value algo_perf(kObjectType);
    for (int a = 0; a != xmrig::PerfAlgo::PA_MAX; ++ a) {
        const xmrig::PerfAlgo pa = static_cast<xmrig::PerfAlgo>(a);
        Value key(xmrig::Algorithm::perfAlgoName(pa), allocator);
        algo_perf.AddMember(key, Value(m_algo_perf[pa]), allocator);
    }
    doc.AddMember("algo-perf", algo_perf, allocator);

    doc.AddMember("calibrate-algo", isCalibrateAlgo(), allocator);
    doc.AddMember("calibrate-algo-time", calibrateAlgoTime(), allocator);

    doc.AddMember("user-agent", userAgent() ? Value(StringRef(userAgent())).Move() : Value(kNullType).Move(), allocator);

#   ifdef HAVE_SYSLOG_H
    doc.AddMember("syslog", isSyslog(), allocator);
#   endif

    doc.AddMember("watch", m_watch, allocator);
}


xmrig::Config *xmrig::Config::load(Process *process, IConfigListener *listener)
{
    return static_cast<Config*>(ConfigLoader::load(process, new ConfigCreator(), listener));
}


bool xmrig::Config::finalize()
{
    if (m_state != NoneState) {
        return CommonConfig::finalize();
    }

    if (!CommonConfig::finalize()) {
        return false;
    }

    // auto configure m_threads
    for (int a = 0; a != xmrig::Algo::ALGO_MAX; ++ a) {
        const xmrig::Algo algo = static_cast<xmrig::Algo>(a);
        if (!m_threads[algo].cpu.empty()) {
            m_threads[algo].mode = Advanced;
            const bool softAES = (m_aesMode == AES_AUTO ? (Cpu::info()->hasAES() ? AES_HW : AES_SOFT) : m_aesMode) == AES_SOFT;
            for (size_t i = 0; i < m_threads[algo].cpu.size(); ++i) {
                m_threads[algo].list.push_back(CpuThread::createFromData(i, algo, m_threads[algo].cpu[i], m_priority, softAES));
            }
        } else {
            const AlgoVariant av = getAlgoVariant();
            m_threads[algo].mode = m_threads[algo].count ? Simple : Automatic;

            const size_t size = CpuThread::multiway(av) * cn_select_memory(algo) / 1024;

            if (!m_threads[algo].count) {
                m_threads[algo].count = Cpu::info()->optimalThreadsCount(size, m_maxCpuUsage);
            }
            else if (m_safe) {
                const size_t count = Cpu::info()->optimalThreadsCount(size, m_maxCpuUsage);
                if (m_threads[algo].count > count) {
                    m_threads[algo].count = count;
                }
            }

            for (size_t i = 0; i < m_threads[algo].count; ++i) {
                m_threads[algo].list.push_back(CpuThread::createFromAV(i, algo, av, m_threads[algo].mask, m_priority, m_assembly));
            }

            m_shouldSave = m_shouldSave || m_threads[algo].mode == Automatic;
        }
    }

    return true;
}


bool xmrig::Config::parseBoolean(int key, bool enable)
{
    if (!CommonConfig::parseBoolean(key, enable)) {
        return false;
    }

    switch (key) {
    case SafeKey: /* --safe */
        m_safe = enable;
        break;

    case HugePagesKey: /* --no-huge-pages */
        m_hugePages = enable;
        break;

    case HardwareAESKey: /* hw-aes config only */
        m_aesMode = enable ? AES_HW : AES_SOFT;
        break;

#   ifndef XMRIG_NO_ASM
    case AssemblyKey:
        m_assembly = Asm::parse(enable);
        break;
#   endif

    default:
        break;
    }

    return true;
}


bool xmrig::Config::parseString(int key, const char *arg)
{
    if (!CommonConfig::parseString(key, arg)) {
        return false;
    }

    switch (key) {
    case AVKey:          /* --av */
    case MaxCPUUsageKey: /* --max-cpu-usage */
    case CPUPriorityKey: /* --cpu-priority */
        return parseUint64(key, strtol(arg, nullptr, 10));

    case SafeKey: /* --safe */
        return parseBoolean(key, true);

    case HugePagesKey: /* --no-huge-pages */
        return parseBoolean(key, false);

    case ThreadsKey:  /* --threads */
        if (strncmp(arg, "all", 3) == 0) {
            m_threads[m_algorithm.algo()].count = Cpu::info()->threads(); // sets default algo threads
            return true;
        }

        return parseUint64(key, strtol(arg, nullptr, 10));

    case CPUAffinityKey: /* --cpu-affinity */
        {
            const char *p  = strstr(arg, "0x");
            return parseUint64(key, p ? strtoull(p, nullptr, 16) : strtoull(arg, nullptr, 10));
        }

#   ifndef XMRIG_NO_ASM
    case AssemblyKey: /* --asm */
        m_assembly = Asm::parse(arg);
        break;
#   endif

    default:
        break;
    }

    return true;
}


bool xmrig::Config::parseUint64(int key, uint64_t arg)
{
    if (!CommonConfig::parseUint64(key, arg)) {
        return false;
    }

    switch (key) {
    case CPUAffinityKey: /* --cpu-affinity */
        if (arg) {
            m_threads[m_algorithm.algo()].mask = arg; // sets default algo threads
        }
        break;

    default:
        return parseInt(key, static_cast<int>(arg));
    }

    return true;
}


// parse specific perf algo (or generic) threads config
void xmrig::Config::parseThreadsJSON(const rapidjson::Value &threads, const xmrig::Algo algo)
{
    for (const rapidjson::Value &value : threads.GetArray()) {
        if (!value.IsObject()) {
            continue;
        }

        if (value.HasMember("low_power_mode")) {
            auto data = CpuThread::parse(value);

            if (data.valid) {
                m_threads[algo].cpu.push_back(std::move(data));
            }
        }
    }
}

void xmrig::Config::parseJSON(const rapidjson::Document &doc)
{
    CommonConfig::parseJSON(doc);

    const rapidjson::Value &threads = doc["threads"];

    if (threads.IsArray()) {
        // parse generic (old) threads
        parseThreadsJSON(threads, m_algorithm.algo());
    } else if (threads.IsObject()) {
        // parse new specific perf algo threads
        for (int a = 0; a != xmrig::Algo::ALGO_MAX; ++ a) {
            const xmrig::Algo algo = static_cast<xmrig::Algo>(a);
            const rapidjson::Value &threads2 = threads[xmrig::Algorithm::perfAlgoName(xmrig::Algorithm(algo).perf_algo())];
            if (threads2.IsArray()) {
                parseThreadsJSON(threads2, algo);
            }
        }
    }

    const rapidjson::Value &algo_perf = doc["algo-perf"];
    if (algo_perf.IsObject()) {
        for (int a = 0; a != xmrig::PerfAlgo::PA_MAX; ++ a) {
            const xmrig::PerfAlgo pa = static_cast<xmrig::PerfAlgo>(a);
            const rapidjson::Value &key = algo_perf[xmrig::Algorithm::perfAlgoName(pa)];
            if (key.IsDouble()) {
                m_algo_perf[pa] = static_cast<float>(key.GetDouble());
            } else if (key.IsInt()) {
                m_algo_perf[pa] = static_cast<float>(key.GetInt());
            }
        }
    }
}


bool xmrig::Config::parseInt(int key, int arg)
{
    switch (key) {
    case ThreadsKey: /* --threads */
        if (arg >= 0 && arg < 1024) {
            m_threads[m_algorithm.algo()].count = arg; // sets default algo threads
        }
        break;

    case AVKey: /* --av */
        if (arg >= AV_AUTO && arg < AV_MAX) {
            m_algoVariant = static_cast<AlgoVariant>(arg);
        }
        break;

    case MaxCPUUsageKey: /* --max-cpu-usage */
        if (m_maxCpuUsage > 0 && arg <= 100) {
            m_maxCpuUsage = arg;
        }
        break;

    case CPUPriorityKey: /* --cpu-priority */
        if (arg >= 0 && arg <= 5) {
            m_priority = arg;
        }
        break;

    default:
        break;
    }

    return true;
}


xmrig::AlgoVariant xmrig::Config::getAlgoVariant() const
{
#   ifndef XMRIG_NO_AEON
    if (m_algorithm.algo() == xmrig::CRYPTONIGHT_LITE) {
        return getAlgoVariantLite();
    }
#   endif

    if (m_algoVariant <= AV_AUTO || m_algoVariant >= AV_MAX) {
        return Cpu::info()->hasAES() ? AV_SINGLE : AV_SINGLE_SOFT;
    }

    if (m_safe && !Cpu::info()->hasAES() && m_algoVariant <= AV_DOUBLE) {
        return static_cast<AlgoVariant>(m_algoVariant + 2);
    }

    return m_algoVariant;
}


#ifndef XMRIG_NO_AEON
xmrig::AlgoVariant xmrig::Config::getAlgoVariantLite() const
{
    if (m_algoVariant <= AV_AUTO || m_algoVariant >= AV_MAX) {
        return Cpu::info()->hasAES() ? AV_DOUBLE : AV_DOUBLE_SOFT;
    }

    if (m_safe && !Cpu::info()->hasAES() && m_algoVariant <= AV_DOUBLE) {
        return static_cast<AlgoVariant>(m_algoVariant + 2);
    }

    return m_algoVariant;
}
#endif
