#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

enum class ProbePoint : uint16_t {
    Unknown = 0,
    ExSetTimer = 1,
    KeWaitForSingleObjectEnter = 2,
    KeWaitForSingleObjectExit = 3,
    KeQueryUnbiasedInterruptTime = 4,
};

enum NoteFlags : uint16_t {
    NoteFlagNone = 0x0000,
    NoteFlagPeriodicTimer = 0x0001,
    NoteFlagOneShotTimer = 0x0002,
    NoteFlagUnmatchedWake = 0x0004,
    NoteFlagCrossCpu = 0x0008,
    NoteFlagTruncatedContext = 0x0010,
    NoteFlagBufferPressureObserved = 0x0020,
    NoteFlagPostWakeSample = 0x0040,
};

enum class MatchConfidence : uint8_t {
    Unmatched = 0,
    Low = 1,
    Medium = 2,
    High = 3,
};

struct ProbeRow {
    size_t lineNumber = 0;
    uint64_t sequence = 0;
    ProbePoint probePoint = ProbePoint::Unknown;
    uint64_t timestamp = 0;
    int64_t dueTime = 0;
    uint32_t timerId = 0;
    uint32_t matchedArmSequenceHint = 0;
    uint32_t contextTag = 0;
    uint32_t threadTag = 0;
    uint16_t noteFlags = 0;
    uint8_t irql = 0;
    uint8_t cpu = 0;
    uint32_t period = 0;
    uint32_t waitStatus = 0;
    std::string rawLine;
};

struct CorrelatedChain {
    MatchConfidence confidence = MatchConfidence::Unmatched;
    const ProbeRow* armRow = nullptr;
    const ProbeRow* waitEnterRow = nullptr;
    const ProbeRow* waitExitRow = nullptr;
    const ProbeRow* sampleRow = nullptr;
    std::optional<int64_t> armToEnterDelta;
    std::optional<int64_t> armToExitDelta;
    std::optional<int64_t> enterToExitDelta;
    std::optional<int64_t> sampleToExitDelta;
    uint64_t cadenceWindowId = 0;
    std::optional<double> cadenceUpperHz;
};

struct CadenceWindow {
    uint64_t windowId = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    double upperHz = 0.0;
};

struct ParseIssue {
    size_t lineNumber = 0;
    std::string message;
    std::string line;
};

struct ParseSummary {
    std::vector<ProbeRow> rows;
    std::vector<ParseIssue> issues;
};

struct LatencyStats {
    bool hasData = false;
    int64_t min = 0;
    int64_t p50 = 0;
    int64_t p95 = 0;
    int64_t max = 0;
};

struct RollupSummary {
    size_t totalRows = 0;
    size_t parsedRows = 0;
    size_t malformedRows = 0;
    size_t totalArms = 0;
    size_t completedChains = 0;
    size_t unmatchedArms = 0;
    size_t unmatchedWaitEnters = 0;
    size_t unmatchedWaitExits = 0;
    size_t highCount = 0;
    size_t mediumCount = 0;
    size_t lowCount = 0;
    size_t unmatchedCount = 0;
    size_t crossCpuChainCount = 0;
    size_t cadenceMatchedChains = 0;
    size_t strongestCadenceWindowId = 0;
    size_t strongestCadenceWindowHits = 0;
    size_t wakeBucketWidth = 0;
    size_t maxBucketDensity = 0;
    LatencyStats armToEnter;
    LatencyStats armToExit;
    LatencyStats enterToExit;
};

struct ToolConfig {
    std::filesystem::path eventsPath;
    std::filesystem::path correlatedPath;
    std::filesystem::path rollupPath;
    std::filesystem::path diagnosticsPath;
    std::optional<std::filesystem::path> cadencePath;
};

static std::string Trim(const std::string& text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

static std::string ToUpper(std::string text)
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
    return text;
}

static std::vector<std::string> SplitByWhitespace(const std::string& line)
{
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    std::string token;

    while (stream >> token) {
        tokens.push_back(token);
    }

    return tokens;
}

static std::vector<std::string> SplitByChar(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::string current;

    for (char ch : text) {
        if (ch == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    parts.push_back(current);
    return parts;
}

static bool ParseUnsigned64(const std::string& text, uint64_t* value)
{
    if (text.empty()) {
        return false;
    }

    const int base = text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
    const char* begin = text.data();
    const char* parseBegin = begin;
    if (base == 16) {
        parseBegin += 2;
    }

    uint64_t parsed = 0;
    const auto result = std::from_chars(parseBegin, begin + text.size(), parsed, base);
    if (result.ec != std::errc() || result.ptr != begin + text.size()) {
        return false;
    }

    *value = parsed;
    return true;
}

static bool ParseSigned64(const std::string& text, int64_t* value)
{
    if (text.empty()) {
        return false;
    }

    const char* begin = text.data();
    int base = 10;
    if (text.size() > 3 && text[0] == '-' && text[1] == '0' &&
        (text[2] == 'x' || text[2] == 'X')) {
        base = 16;
    } else if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }

    bool negative = false;
    const char* parseBegin = begin;
    if (*parseBegin == '-') {
        negative = true;
        ++parseBegin;
    }
    if (base == 16 && parseBegin[0] == '0' &&
        (parseBegin[1] == 'x' || parseBegin[1] == 'X')) {
        parseBegin += 2;
    }

    uint64_t magnitude = 0;
    const auto result = std::from_chars(parseBegin, begin + text.size(), magnitude, base);
    if (result.ec != std::errc() || result.ptr != begin + text.size()) {
        return false;
    }

    if (negative) {
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ull) {
            return false;
        }
        *value = -static_cast<int64_t>(magnitude);
    } else {
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return false;
        }
        *value = static_cast<int64_t>(magnitude);
    }

    return true;
}

static bool ParseUnsigned32(const std::string& text, uint32_t* value)
{
    uint64_t parsed = 0;
    if (!ParseUnsigned64(text, &parsed) || parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    *value = static_cast<uint32_t>(parsed);
    return true;
}

static bool ParseUnsigned16(const std::string& text, uint16_t* value)
{
    uint64_t parsed = 0;
    if (!ParseUnsigned64(text, &parsed) || parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    *value = static_cast<uint16_t>(parsed);
    return true;
}

static bool ParseUnsigned8(const std::string& text, uint8_t* value)
{
    uint64_t parsed = 0;
    if (!ParseUnsigned64(text, &parsed) || parsed > std::numeric_limits<uint8_t>::max()) {
        return false;
    }

    *value = static_cast<uint8_t>(parsed);
    return true;
}

static bool ParseDoubleValue(const std::string& text, double* value)
{
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }

    *value = parsed;
    return true;
}

static bool ParseProbePoint(const std::string& text, ProbePoint* probePoint)
{
    const std::string upper = ToUpper(text);
    if (upper == "EXSETTIMER" || upper == "1") {
        *probePoint = ProbePoint::ExSetTimer;
        return true;
    }
    if (upper == "KEWAITFORSINGLEOBJECTENTER" || upper == "2") {
        *probePoint = ProbePoint::KeWaitForSingleObjectEnter;
        return true;
    }
    if (upper == "KEWAITFORSINGLEOBJECTEXIT" || upper == "3") {
        *probePoint = ProbePoint::KeWaitForSingleObjectExit;
        return true;
    }
    if (upper == "KEQUERYUNBIASEDINTERRUPTTIME" || upper == "4") {
        *probePoint = ProbePoint::KeQueryUnbiasedInterruptTime;
        return true;
    }

    return false;
}

static bool ParseNoteFlags(const std::string& text, uint16_t* flags)
{
    uint16_t parsedFlags = 0;
    uint16_t numericValue = 0;
    if (ParseUnsigned16(text, &numericValue)) {
        *flags = numericValue;
        return true;
    }

    const auto parts = SplitByChar(text, '|');
    for (const std::string& rawPart : parts) {
        const std::string upperPart = ToUpper(Trim(rawPart));
        if (upperPart.empty()) {
            continue;
        }

        if (upperPart == "PERIODIC" || upperPart == "PERIODICTIMER") {
            parsedFlags |= NoteFlagPeriodicTimer;
        } else if (upperPart == "ONESHOT" || upperPart == "ONESHOTTIMER") {
            parsedFlags |= NoteFlagOneShotTimer;
        } else if (upperPart == "UNMATCHEDWAKE") {
            parsedFlags |= NoteFlagUnmatchedWake;
        } else if (upperPart == "CROSSCPU") {
            parsedFlags |= NoteFlagCrossCpu;
        } else if (upperPart == "TRUNCATEDCONTEXT") {
            parsedFlags |= NoteFlagTruncatedContext;
        } else if (upperPart == "BUFFERPRESSUREOBSERVED") {
            parsedFlags |= NoteFlagBufferPressureObserved;
        } else if (upperPart == "POSTWAKESAMPLE") {
            parsedFlags |= NoteFlagPostWakeSample;
        } else {
            return false;
        }
    }

    *flags = parsedFlags;
    return true;
}

static std::string MatchConfidenceName(MatchConfidence confidence)
{
    switch (confidence) {
    case MatchConfidence::High:
        return "high";
    case MatchConfidence::Medium:
        return "medium";
    case MatchConfidence::Low:
        return "low";
    case MatchConfidence::Unmatched:
    default:
        return "unmatched";
    }
}

static std::string FormatOptionalInt64(const std::optional<int64_t>& value)
{
    if (!value.has_value()) {
        return "n/a";
    }

    return std::to_string(*value);
}

static bool ParseEventRow(
    const std::string& line,
    size_t lineNumber,
    ProbeRow* row,
    std::string* errorMessage)
{
    ProbeRow parsedRow = {};
    parsedRow.lineNumber = lineNumber;
    parsedRow.rawLine = line;

    bool hasSequence = false;
    bool hasProbePoint = false;
    bool hasTimestamp = false;

    const std::vector<std::string> tokens = SplitByWhitespace(line);
    for (const std::string& token : tokens) {
        const size_t separator = token.find('=');
        if (separator == std::string::npos) {
            *errorMessage = "token is not key=value: " + token;
            return false;
        }

        const std::string key = ToUpper(token.substr(0, separator));
        const std::string value = token.substr(separator + 1);

        if (key == "SEQUENCE") {
            hasSequence = ParseUnsigned64(value, &parsedRow.sequence);
            if (!hasSequence) {
                *errorMessage = "invalid sequence";
                return false;
            }
        } else if (key == "PROBE_POINT") {
            hasProbePoint = ParseProbePoint(value, &parsedRow.probePoint);
            if (!hasProbePoint) {
                *errorMessage = "invalid probe_point";
                return false;
            }
        } else if (key == "TIMESTAMP_QPC_LIKE") {
            hasTimestamp = ParseUnsigned64(value, &parsedRow.timestamp);
            if (!hasTimestamp) {
                *errorMessage = "invalid timestamp_qpc_like";
                return false;
            }
        } else if (key == "IRQL") {
            if (!ParseUnsigned8(value, &parsedRow.irql)) {
                *errorMessage = "invalid irql";
                return false;
            }
        } else if (key == "CPU") {
            if (!ParseUnsigned8(value, &parsedRow.cpu)) {
                *errorMessage = "invalid cpu";
                return false;
            }
        } else if (key == "CONTEXT_TAG") {
            if (!ParseUnsigned32(value, &parsedRow.contextTag)) {
                *errorMessage = "invalid context_tag";
                return false;
            }
        } else if (key == "THREAD_TAG") {
            if (!ParseUnsigned32(value, &parsedRow.threadTag)) {
                *errorMessage = "invalid thread_tag";
                return false;
            }
        } else if (key == "TIMER_ID") {
            if (!ParseUnsigned32(value, &parsedRow.timerId)) {
                *errorMessage = "invalid timer_id";
                return false;
            }
        } else if (key == "DUE_TIME") {
            if (!ParseSigned64(value, &parsedRow.dueTime)) {
                *errorMessage = "invalid due_time";
                return false;
            }
        } else if (key == "PERIOD") {
            if (!ParseUnsigned32(value, &parsedRow.period)) {
                *errorMessage = "invalid period";
                return false;
            }
        } else if (key == "WAIT_STATUS") {
            if (!ParseUnsigned32(value, &parsedRow.waitStatus)) {
                *errorMessage = "invalid wait_status";
                return false;
            }
        } else if (key == "MATCHED_ARM_SEQUENCE" || key == "MATCHED_ARM_SEQUENCE_HINT") {
            if (!ParseUnsigned32(value, &parsedRow.matchedArmSequenceHint)) {
                *errorMessage = "invalid matched_arm_sequence";
                return false;
            }
        } else if (key == "NOTE_FLAGS") {
            if (!ParseNoteFlags(value, &parsedRow.noteFlags)) {
                *errorMessage = "invalid note_flags";
                return false;
            }
        }
    }

    if (!hasSequence || !hasProbePoint || !hasTimestamp) {
        *errorMessage = "missing one of sequence/probe_point/timestamp_qpc_like";
        return false;
    }

    *row = parsedRow;
    return true;
}

static ParseSummary LoadEventRows(const std::filesystem::path& path)
{
    ParseSummary summary = {};
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open events file: " + path.string());
    }

    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        ProbeRow row = {};
        std::string errorMessage;
        if (!ParseEventRow(trimmed, lineNumber, &row, &errorMessage)) {
            summary.issues.push_back(ParseIssue{ lineNumber, errorMessage, trimmed });
            continue;
        }

        summary.rows.push_back(row);
    }

    std::sort(
        summary.rows.begin(),
        summary.rows.end(),
        [](const ProbeRow& left, const ProbeRow& right) {
            if (left.sequence != right.sequence) {
                return left.sequence < right.sequence;
            }
            return left.lineNumber < right.lineNumber;
        });

    return summary;
}

static std::vector<CadenceWindow> LoadCadenceWindows(const std::filesystem::path& path)
{
    std::vector<CadenceWindow> windows;
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open cadence file: " + path.string());
    }

    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        if (lineNumber == 1 && trimmed.find("window_id") != std::string::npos &&
            trimmed.find(',') != std::string::npos) {
            continue;
        }

        CadenceWindow window = {};
        bool parsed = false;

        if (trimmed.find('=') != std::string::npos) {
            const auto tokens = SplitByWhitespace(trimmed);
            bool hasId = false;
            bool hasStart = false;
            bool hasEnd = false;
            bool hasHz = false;
            for (const std::string& token : tokens) {
                const size_t separator = token.find('=');
                if (separator == std::string::npos) {
                    continue;
                }

                const std::string key = ToUpper(token.substr(0, separator));
                const std::string value = token.substr(separator + 1);
                if (key == "WINDOW_ID") {
                    hasId = ParseUnsigned64(value, &window.windowId);
                } else if (key == "START") {
                    hasStart = ParseUnsigned64(value, &window.start);
                } else if (key == "END") {
                    hasEnd = ParseUnsigned64(value, &window.end);
                } else if (key == "UPPER_HZ") {
                    hasHz = ParseDoubleValue(value, &window.upperHz);
                }
            }
            parsed = hasId && hasStart && hasEnd && hasHz;
        } else if (trimmed.find(',') != std::string::npos) {
            const auto columns = SplitByChar(trimmed, ',');
            if (columns.size() >= 4) {
                parsed =
                    ParseUnsigned64(Trim(columns[0]), &window.windowId) &&
                    ParseUnsigned64(Trim(columns[1]), &window.start) &&
                    ParseUnsigned64(Trim(columns[2]), &window.end) &&
                    ParseDoubleValue(Trim(columns[3]), &window.upperHz);
            }
        }

        if (!parsed) {
            throw std::runtime_error("Failed to parse cadence line " + std::to_string(lineNumber));
        }

        windows.push_back(window);
    }

    return windows;
}

struct MatchResult {
    MatchConfidence confidence = MatchConfidence::Unmatched;
    const ProbeRow* armRow = nullptr;
};

static MatchResult FindBestArm(
    const ProbeRow& row,
    const std::vector<const ProbeRow*>& arms,
    const std::unordered_map<uint32_t, std::vector<const ProbeRow*>>& armsByTimerId)
{
    auto latestPriorArm = [&](auto predicate) -> const ProbeRow* {
        const ProbeRow* bestArm = nullptr;
        for (const ProbeRow* candidate : arms) {
            if (candidate->sequence > row.sequence || !predicate(*candidate)) {
                continue;
            }
            if (bestArm == nullptr || candidate->sequence > bestArm->sequence) {
                bestArm = candidate;
            }
        }
        return bestArm;
    };

    if (row.timerId != 0) {
        const auto armIter = armsByTimerId.find(row.timerId);
        if (armIter != armsByTimerId.end()) {
            const ProbeRow* bestArm = nullptr;
            for (const ProbeRow* candidate : armIter->second) {
                if (candidate->sequence > row.sequence) {
                    continue;
                }
                if (bestArm == nullptr || candidate->sequence > bestArm->sequence) {
                    bestArm = candidate;
                }
            }
            if (bestArm != nullptr) {
                const MatchConfidence confidence =
                    row.contextTag != 0 && bestArm->contextTag == row.contextTag
                    ? MatchConfidence::High
                    : MatchConfidence::Medium;
                return MatchResult{ confidence, bestArm };
            }
        }
    }

    if (row.contextTag != 0) {
        const ProbeRow* bestArm = latestPriorArm([&](const ProbeRow& candidate) {
            return candidate.contextTag == row.contextTag;
        });
        if (bestArm != nullptr) {
            return MatchResult{ MatchConfidence::Low, bestArm };
        }
    }

    const ProbeRow* bestArm = latestPriorArm([&](const ProbeRow& candidate) {
        return candidate.cpu == row.cpu;
    });
    if (bestArm != nullptr) {
        return MatchResult{ MatchConfidence::Low, bestArm };
    }

    return MatchResult{};
}

static const CadenceWindow* FindNearestCadenceWindow(
    uint64_t timestamp,
    const std::vector<CadenceWindow>& windows)
{
    const CadenceWindow* bestWindow = nullptr;
    uint64_t bestDistance = std::numeric_limits<uint64_t>::max();

    for (const CadenceWindow& window : windows) {
        if (timestamp >= window.start && timestamp <= window.end) {
            return &window;
        }

        uint64_t distance = timestamp < window.start
            ? window.start - timestamp
            : timestamp - window.end;
        if (bestWindow == nullptr || distance < bestDistance) {
            bestWindow = &window;
            bestDistance = distance;
        }
    }

    return bestWindow;
}

static std::vector<CorrelatedChain> BuildCorrelatedChains(
    const std::vector<ProbeRow>& rows,
    size_t* unmatchedWaitEnters,
    size_t* unmatchedWaitExits)
{
    std::vector<const ProbeRow*> armRows;
    std::unordered_map<uint32_t, std::vector<const ProbeRow*>> armsByTimerId;
    std::unordered_map<uint64_t, std::vector<std::pair<const ProbeRow*, MatchConfidence>>> enterRowsByArm;
    std::unordered_map<uint64_t, std::vector<std::pair<const ProbeRow*, MatchConfidence>>> exitRowsByArm;
    std::unordered_map<uint64_t, std::vector<const ProbeRow*>> sampleRowsByArm;

    for (const ProbeRow& row : rows) {
        if (row.probePoint == ProbePoint::ExSetTimer) {
            armRows.push_back(&row);
            if (row.timerId != 0) {
                armsByTimerId[row.timerId].push_back(&row);
            }
        }
    }

    for (const ProbeRow& row : rows) {
        if (row.probePoint == ProbePoint::KeWaitForSingleObjectEnter ||
            row.probePoint == ProbePoint::KeWaitForSingleObjectExit ||
            row.probePoint == ProbePoint::KeQueryUnbiasedInterruptTime) {
            const MatchResult match = FindBestArm(row, armRows, armsByTimerId);
            if (match.armRow == nullptr) {
                if (row.probePoint == ProbePoint::KeWaitForSingleObjectEnter) {
                    ++(*unmatchedWaitEnters);
                } else if (row.probePoint == ProbePoint::KeWaitForSingleObjectExit) {
                    ++(*unmatchedWaitExits);
                }
                continue;
            }

            const uint64_t armSequence = match.armRow->sequence;
            if (row.probePoint == ProbePoint::KeWaitForSingleObjectEnter) {
                enterRowsByArm[armSequence].push_back(std::make_pair(&row, match.confidence));
            } else if (row.probePoint == ProbePoint::KeWaitForSingleObjectExit) {
                exitRowsByArm[armSequence].push_back(std::make_pair(&row, match.confidence));
            } else {
                sampleRowsByArm[armSequence].push_back(&row);
            }
        }
    }

    std::vector<CorrelatedChain> chains;
    chains.reserve(armRows.size());
    for (const ProbeRow* armRow : armRows) {
        CorrelatedChain chain = {};
        chain.armRow = armRow;

        auto enterIter = enterRowsByArm.find(armRow->sequence);
        if (enterIter != enterRowsByArm.end() && !enterIter->second.empty()) {
            std::sort(
                enterIter->second.begin(),
                enterIter->second.end(),
                [](const auto& left, const auto& right) {
                    return left.first->sequence < right.first->sequence;
                });
            chain.waitEnterRow = enterIter->second.front().first;
            chain.confidence = enterIter->second.front().second;
        }

        auto exitIter = exitRowsByArm.find(armRow->sequence);
        if (exitIter != exitRowsByArm.end() && !exitIter->second.empty()) {
            std::sort(
                exitIter->second.begin(),
                exitIter->second.end(),
                [](const auto& left, const auto& right) {
                    return left.first->sequence < right.first->sequence;
                });
            const uint64_t minimumSequence = chain.waitEnterRow != nullptr
                ? chain.waitEnterRow->sequence
                : armRow->sequence;
            for (const auto& exitCandidate : exitIter->second) {
                if (exitCandidate.first->sequence >= minimumSequence) {
                    chain.waitExitRow = exitCandidate.first;
                    chain.confidence = chain.confidence == MatchConfidence::Unmatched
                        ? exitCandidate.second
                        : static_cast<MatchConfidence>(
                            std::min(
                                static_cast<int>(chain.confidence),
                                static_cast<int>(exitCandidate.second)));
                    break;
                }
            }
        }

        auto sampleIter = sampleRowsByArm.find(armRow->sequence);
        if (sampleIter != sampleRowsByArm.end() && !sampleIter->second.empty()) {
            std::sort(
                sampleIter->second.begin(),
                sampleIter->second.end(),
                [](const ProbeRow* left, const ProbeRow* right) {
                    return left->sequence < right->sequence;
                });
            const uint64_t minimumSequence = chain.waitExitRow != nullptr
                ? chain.waitExitRow->sequence
                : armRow->sequence;
            for (const ProbeRow* sampleCandidate : sampleIter->second) {
                if (sampleCandidate->sequence >= minimumSequence) {
                    chain.sampleRow = sampleCandidate;
                    break;
                }
            }
        }

        if (chain.waitEnterRow != nullptr) {
            chain.armToEnterDelta = static_cast<int64_t>(chain.waitEnterRow->timestamp - armRow->timestamp);
        }
        if (chain.waitExitRow != nullptr) {
            chain.armToExitDelta = static_cast<int64_t>(chain.waitExitRow->timestamp - armRow->timestamp);
        }
        if (chain.waitEnterRow != nullptr && chain.waitExitRow != nullptr) {
            chain.enterToExitDelta = static_cast<int64_t>(chain.waitExitRow->timestamp - chain.waitEnterRow->timestamp);
        }
        if (chain.waitExitRow != nullptr && chain.sampleRow != nullptr) {
            chain.sampleToExitDelta = static_cast<int64_t>(chain.sampleRow->timestamp - chain.waitExitRow->timestamp);
        }

        chains.push_back(chain);
    }

    return chains;
}

static void AttachCadenceWindows(
    std::vector<CorrelatedChain>* chains,
    const std::vector<CadenceWindow>& windows)
{
    if (windows.empty()) {
        return;
    }

    for (CorrelatedChain& chain : *chains) {
        const uint64_t anchorTimestamp = chain.waitExitRow != nullptr
            ? chain.waitExitRow->timestamp
            : chain.armRow->timestamp;
        const CadenceWindow* window = FindNearestCadenceWindow(anchorTimestamp, windows);
        if (window != nullptr) {
            chain.cadenceWindowId = window->windowId;
            chain.cadenceUpperHz = window->upperHz;
        }
    }
}

static LatencyStats ComputeLatencyStats(std::vector<int64_t> values)
{
    LatencyStats stats = {};
    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end());
    auto percentileValue = [&](double percentile) -> int64_t {
        const double scaledIndex = percentile * static_cast<double>(values.size() - 1);
        const size_t index = static_cast<size_t>(std::llround(scaledIndex));
        return values[index];
    };

    stats.hasData = true;
    stats.min = values.front();
    stats.p50 = percentileValue(0.50);
    stats.p95 = percentileValue(0.95);
    stats.max = values.back();
    return stats;
}

static RollupSummary ComputeRollup(
    const ParseSummary& parseSummary,
    const std::vector<CorrelatedChain>& chains)
{
    RollupSummary summary = {};
    summary.totalRows = parseSummary.rows.size() + parseSummary.issues.size();
    summary.parsedRows = parseSummary.rows.size();
    summary.malformedRows = parseSummary.issues.size();

    std::vector<int64_t> armToEnterDeltas;
    std::vector<int64_t> armToExitDeltas;
    std::vector<int64_t> enterToExitDeltas;
    std::map<uint64_t, size_t> cadenceWindowHits;
    std::map<uint64_t, size_t> wakeBuckets;

    for (const CorrelatedChain& chain : chains) {
        ++summary.totalArms;
        if (chain.waitExitRow != nullptr) {
            ++summary.completedChains;
        } else {
            ++summary.unmatchedArms;
        }

        switch (chain.confidence) {
        case MatchConfidence::High:
            ++summary.highCount;
            break;
        case MatchConfidence::Medium:
            ++summary.mediumCount;
            break;
        case MatchConfidence::Low:
            ++summary.lowCount;
            break;
        case MatchConfidence::Unmatched:
        default:
            ++summary.unmatchedCount;
            break;
        }

        if (chain.waitEnterRow != nullptr && chain.waitExitRow != nullptr &&
            chain.waitEnterRow->cpu != chain.waitExitRow->cpu) {
            ++summary.crossCpuChainCount;
        } else if (chain.armRow != nullptr && chain.waitExitRow != nullptr &&
            chain.armRow->cpu != chain.waitExitRow->cpu) {
            ++summary.crossCpuChainCount;
        }

        if (chain.armToEnterDelta.has_value()) {
            armToEnterDeltas.push_back(*chain.armToEnterDelta);
        }
        if (chain.armToExitDelta.has_value()) {
            armToExitDeltas.push_back(*chain.armToExitDelta);
        }
        if (chain.enterToExitDelta.has_value()) {
            enterToExitDeltas.push_back(*chain.enterToExitDelta);
        }

        if (chain.cadenceWindowId != 0) {
            ++summary.cadenceMatchedChains;
            ++cadenceWindowHits[chain.cadenceWindowId];
        }

        if (chain.waitExitRow != nullptr) {
            const uint64_t bucketWidth = 10000;
            const uint64_t bucketId = chain.waitExitRow->timestamp / bucketWidth;
            ++wakeBuckets[bucketId];
        }
    }

    summary.armToEnter = ComputeLatencyStats(armToEnterDeltas);
    summary.armToExit = ComputeLatencyStats(armToExitDeltas);
    summary.enterToExit = ComputeLatencyStats(enterToExitDeltas);
    summary.wakeBucketWidth = 10000;

    for (const auto& [bucketId, bucketCount] : wakeBuckets) {
        (void)bucketId;
        summary.maxBucketDensity = std::max(summary.maxBucketDensity, bucketCount);
    }

    for (const auto& [windowId, hitCount] : cadenceWindowHits) {
        if (hitCount > summary.strongestCadenceWindowHits) {
            summary.strongestCadenceWindowHits = hitCount;
            summary.strongestCadenceWindowId = windowId;
        }
    }

    for (const ProbeRow& row : parseSummary.rows) {
        if (row.probePoint == ProbePoint::KeWaitForSingleObjectEnter) {
            bool matched = false;
            for (const CorrelatedChain& chain : chains) {
                if (chain.waitEnterRow != nullptr &&
                    chain.waitEnterRow->sequence == row.sequence) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                ++summary.unmatchedWaitEnters;
            }
        } else if (row.probePoint == ProbePoint::KeWaitForSingleObjectExit) {
            bool matched = false;
            for (const CorrelatedChain& chain : chains) {
                if (chain.waitExitRow != nullptr &&
                    chain.waitExitRow->sequence == row.sequence) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                ++summary.unmatchedWaitExits;
            }
        }
    }

    return summary;
}

static void WriteCorrelatedChains(
    const std::filesystem::path& path,
    const std::vector<CorrelatedChain>& chains)
{
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("Failed to write correlated output: " + path.string());
    }

    output << "# UsbXhciObservation Correlated Chains\n";
    for (const CorrelatedChain& chain : chains) {
        output
            << "arm_sequence=" << chain.armRow->sequence
            << " confidence=" << MatchConfidenceName(chain.confidence)
            << " timer_id=" << chain.armRow->timerId
            << " context_tag=" << chain.armRow->contextTag
            << " cpu=" << static_cast<unsigned>(chain.armRow->cpu)
            << " arm_timestamp=" << chain.armRow->timestamp
            << " wait_enter_sequence=" << (chain.waitEnterRow != nullptr ? std::to_string(chain.waitEnterRow->sequence) : "n/a")
            << " wait_exit_sequence=" << (chain.waitExitRow != nullptr ? std::to_string(chain.waitExitRow->sequence) : "n/a")
            << " sample_sequence=" << (chain.sampleRow != nullptr ? std::to_string(chain.sampleRow->sequence) : "n/a")
            << " arm_to_enter=" << FormatOptionalInt64(chain.armToEnterDelta)
            << " arm_to_exit=" << FormatOptionalInt64(chain.armToExitDelta)
            << " enter_to_exit=" << FormatOptionalInt64(chain.enterToExitDelta)
            << " sample_to_exit=" << FormatOptionalInt64(chain.sampleToExitDelta)
            << " cadence_window_id=" << (chain.cadenceWindowId != 0 ? std::to_string(chain.cadenceWindowId) : "n/a")
            << " cadence_upper_hz=" << (chain.cadenceUpperHz.has_value() ? std::to_string(*chain.cadenceUpperHz) : "n/a")
            << "\n";
    }
}

static void WriteLatencyStats(
    std::ofstream& output,
    const char* label,
    const LatencyStats& stats)
{
    output << label << ": ";
    if (!stats.hasData) {
        output << "n/a\n";
        return;
    }

    output
        << "min=" << stats.min
        << " p50=" << stats.p50
        << " p95=" << stats.p95
        << " max=" << stats.max
        << "\n";
}

static void WriteRollup(
    const std::filesystem::path& path,
    const RollupSummary& summary)
{
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("Failed to write rollup output: " + path.string());
    }

    output << "# UsbXhciObservation Rollup\n";
    output << "total_rows=" << summary.totalRows << "\n";
    output << "parsed_rows=" << summary.parsedRows << "\n";
    output << "malformed_rows=" << summary.malformedRows << "\n";
    output << "total_arms=" << summary.totalArms << "\n";
    output << "completed_chains=" << summary.completedChains << "\n";
    output << "unmatched_arms=" << summary.unmatchedArms << "\n";
    output << "unmatched_wait_enters=" << summary.unmatchedWaitEnters << "\n";
    output << "unmatched_wait_exits=" << summary.unmatchedWaitExits << "\n\n";

    output << "[confidence]\n";
    output << "high=" << summary.highCount << "\n";
    output << "medium=" << summary.mediumCount << "\n";
    output << "low=" << summary.lowCount << "\n";
    output << "unmatched=" << summary.unmatchedCount << "\n\n";

    output << "[latency]\n";
    WriteLatencyStats(output, "arm_to_enter", summary.armToEnter);
    WriteLatencyStats(output, "arm_to_exit", summary.armToExit);
    WriteLatencyStats(output, "enter_to_exit", summary.enterToExit);
    output << "\n";

    output << "[clustering]\n";
    output << "wake_bucket_width=" << summary.wakeBucketWidth << "\n";
    output << "max_bucket_density=" << summary.maxBucketDensity << "\n";
    output << "cross_cpu_chain_count=" << summary.crossCpuChainCount << "\n\n";

    output << "[cadence]\n";
    output << "cadence_matched_chains=" << summary.cadenceMatchedChains << "\n";
    output << "strongest_window_id=" << summary.strongestCadenceWindowId << "\n";
    output << "strongest_window_hits=" << summary.strongestCadenceWindowHits << "\n";
}

static void WriteDiagnostics(
    const std::filesystem::path& path,
    const ParseSummary& parseSummary,
    const std::vector<CorrelatedChain>& chains)
{
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("Failed to write diagnostics output: " + path.string());
    }

    output << "# UsbXhciObservation Diagnostics\n";
    output << "[parse_issues]\n";
    if (parseSummary.issues.empty()) {
        output << "none\n";
    } else {
        for (const ParseIssue& issue : parseSummary.issues) {
            output
                << "line=" << issue.lineNumber
                << " message=\"" << issue.message << "\""
                << " raw=\"" << issue.line << "\"\n";
        }
    }

    output << "\n[unmatched_arms]\n";
    bool wroteUnmatchedArm = false;
    for (const CorrelatedChain& chain : chains) {
        if (chain.waitExitRow == nullptr) {
            wroteUnmatchedArm = true;
            output
                << "arm_sequence=" << chain.armRow->sequence
                << " timer_id=" << chain.armRow->timerId
                << " context_tag=" << chain.armRow->contextTag
                << " confidence=" << MatchConfidenceName(chain.confidence)
                << "\n";
        }
    }
    if (!wroteUnmatchedArm) {
        output << "none\n";
    }
}

static std::filesystem::path DeriveOutputPath(
    const std::filesystem::path& eventsPath,
    const char* suffix)
{
    const std::filesystem::path parent = eventsPath.parent_path();
    const std::string stem = eventsPath.stem().string();
    return parent / (stem + suffix);
}

static void PrintUsage()
{
    std::printf(
        "ObservationRollup.exe <events_path> [correlated_path] [rollup_path] [diagnostics_path] [cadence_path]\n");
}

static ToolConfig ParseArgs(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("missing events_path");
    }

    ToolConfig config = {};
    config.eventsPath = argv[1];
    config.correlatedPath = argc >= 3
        ? std::filesystem::path(argv[2])
        : DeriveOutputPath(config.eventsPath, "-correlated.txt");
    config.rollupPath = argc >= 4
        ? std::filesystem::path(argv[3])
        : DeriveOutputPath(config.eventsPath, "-rollup.txt");
    config.diagnosticsPath = argc >= 5
        ? std::filesystem::path(argv[4])
        : DeriveOutputPath(config.eventsPath, "-diagnostics.txt");
    if (argc >= 6) {
        config.cadencePath = std::filesystem::path(argv[5]);
    }

    return config;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const ToolConfig config = ParseArgs(argc, argv);
        const ParseSummary parseSummary = LoadEventRows(config.eventsPath);
        size_t unmatchedWaitEnters = 0;
        size_t unmatchedWaitExits = 0;
        std::vector<CorrelatedChain> chains = BuildCorrelatedChains(
            parseSummary.rows,
            &unmatchedWaitEnters,
            &unmatchedWaitExits);

        if (config.cadencePath.has_value()) {
            const std::vector<CadenceWindow> cadenceWindows = LoadCadenceWindows(*config.cadencePath);
            AttachCadenceWindows(&chains, cadenceWindows);
        }

        const RollupSummary rollup = ComputeRollup(parseSummary, chains);
        WriteCorrelatedChains(config.correlatedPath, chains);
        WriteRollup(config.rollupPath, rollup);
        WriteDiagnostics(config.diagnosticsPath, parseSummary, chains);

        std::printf("Parsed rows      : %zu\n", parseSummary.rows.size());
        std::printf("Malformed rows   : %zu\n", parseSummary.issues.size());
        std::printf("Completed chains : %zu\n", rollup.completedChains);
        std::printf("Correlated output: %s\n", config.correlatedPath.string().c_str());
        std::printf("Rollup output    : %s\n", config.rollupPath.string().c_str());
        std::printf("Diagnostics      : %s\n", config.diagnosticsPath.string().c_str());
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "ObservationRollup failed: %s\n", exception.what());
        return 1;
    }
}
