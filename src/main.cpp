#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr int kTicksPerBeatDefault = 480;
    constexpr int kMinLane = 0;
    constexpr int kMaxLane = 11;
    constexpr int kTargetSampleRate = 44100;

    struct SoundDefinition
    {
        const char* file;
        float volume;
        bool loop;
    };

    const std::unordered_map<std::string, SoundDefinition> kSoundDefinitions = {
        {"tap", {"se_live_perfect.mp3", 0.75f, false}},
        {"criticalTap", {"se_live_critical.mp3", 0.75f, false}},
        {"flick", {"se_live_flick.mp3", 0.75f, false}},
        {"flickCritical", {"se_live_flick_critical.mp3", 0.8f, false}},
        {"trace", {"se_live_trace.mp3", 0.8f, false}},
        {"traceCritical", {"se_live_trace_critical.mp3", 0.82f, false}},
        {"tick", {"se_live_connect.mp3", 0.9f, false}},
        {"tickCritical", {"se_live_connect_critical.mp3", 0.92f, false}},
        {"holdLoop", {"se_live_long.mp3", 0.7f, true}},
        {"holdLoopCritical", {"se_live_long_critical.mp3", 0.7f, true}},
    };

    struct CliOptions
    {
        fs::path susPath;
        fs::path outPath;
        fs::path soundRoot;
        std::optional<double> offsetMs;
        std::string format;
    };

    struct SusDataLine
    {
        explicit SusDataLine(int measureOffset, const std::string& line)
            : measureOffset_(measureOffset)
        {
            const size_t separatorIndex = line.find(':');
            if (separatorIndex == std::string::npos)
            {
                throw std::runtime_error("Invalid SUS line: missing ':'");
            }

            header = trim(line.substr(1, separatorIndex - 1));
            data = trim(line.substr(separatorIndex + 1));

            const std::string headerMeasure = header.substr(0, 3);
            if (isDigitString(headerMeasure))
            {
                measure_ = std::atoi(headerMeasure.c_str());
            }
        }

        [[nodiscard]] int getEffectiveMeasure() const
        {
            return measureOffset_ + measure_;
        }

        static std::string trim(const std::string& input)
        {
            const size_t begin = input.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
            {
                return {};
            }
            const size_t end = input.find_last_not_of(" \t\r\n");
            return input.substr(begin, end - begin + 1);
        }

        static bool isDigitString(std::string_view value)
        {
            return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        }

        std::string header;
        std::string data;

    private:
        int measureOffset_{};
        int measure_{};
    };

    struct SUSNote
    {
        int tick{};
        int lane{};
        int width{};
        int type{};
    };

    struct BPM
    {
        int tick{};
        float bpm{};
    };

    struct BarLength
    {
        int bar{};
        float length{};
    };

    struct Bar
    {
        int measure{};
        int ticksPerMeasure{};
        int ticks{};
    };

    struct SUS
    {
        std::unordered_map<std::string, std::string> metadata;
        float waveOffset{};
        std::vector<SUSNote> taps;
        std::vector<SUSNote> directionals;
        std::vector<std::vector<SUSNote>> slides;
        std::vector<std::vector<SUSNote>> guides;
        std::vector<BPM> bpms;
        std::vector<BarLength> barlengths;
    };

    enum class NoteType
    {
        Tap,
        Hold,
        HoldMid,
        HoldEnd,
    };

    enum class FlickType
    {
        None,
        Default,
        Left,
        Right,
    };

    enum class HoldStepType
    {
        Normal,
        Hidden,
        Skip,
    };

    enum class HoldNoteType
    {
        Normal,
        Hidden,
        Guide,
    };

    enum class EaseType
    {
        Linear,
        EaseIn,
        EaseOut,
    };

    struct Tempo
    {
        int tick{};
        float bpm{};
    };

    struct Note
    {
        NoteType type{NoteType::Tap};
        int ID{};
        int parentID{-1};
        int tick{};
        int lane{};
        int width{};
        bool critical{};
        bool friction{};
        FlickType flick{FlickType::None};

        [[nodiscard]] bool isFlick() const
        {
            return flick != FlickType::None;
        }
    };

    struct HoldStep
    {
        int ID{};
        HoldStepType type{HoldStepType::Normal};
        EaseType ease{EaseType::Linear};
    };

    struct HoldNote
    {
        HoldStep start{};
        std::vector<HoldStep> steps;
        int end{};
        HoldNoteType startType{HoldNoteType::Normal};
        HoldNoteType endType{HoldNoteType::Normal};

        [[nodiscard]] bool isGuide() const
        {
            return startType == HoldNoteType::Guide || endType == HoldNoteType::Guide;
        }
    };

    struct Score
    {
        std::map<int, Note> notes;
        std::map<int, HoldNote> holdNotes;
        std::vector<Tempo> tempoChanges;
        float musicOffsetMs{};
    };

    struct HitEvent
    {
        double timeSec{};
        double endTimeSec{-1.0};
        float center{};
        float width{};
        std::string kind;
        bool critical{};
    };

    std::string trim(const std::string& input)
    {
        return SusDataLine::trim(input);
    }

    bool startsWith(std::string_view value, std::string_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    bool endsWith(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    bool isDigitString(std::string_view value)
    {
        return SusDataLine::isDigitString(value);
    }

    std::vector<std::string> splitWhitespace(const std::string& value)
    {
        std::stringstream stream(value);
        std::vector<std::string> parts;
        std::string token;
        while (stream >> token)
        {
            parts.push_back(token);
        }
        return parts;
    }

    std::string noteKey(const SUSNote& note)
    {
        return std::to_string(note.tick) + "-" + std::to_string(note.lane);
    }

    float ticksToSec(int ticks, int beatTicks, float bpm)
    {
        return ticks * (60.0f / bpm / static_cast<float>(beatTicks));
    }

    double accumulateDuration(int tick, int beatTicks, const std::vector<Tempo>& tempos)
    {
        if (tempos.empty())
        {
            return 0.0;
        }

        double total = 0.0;
        int accTicks = 0;
        int lastTempo = 0;
        for (int i = 0; i < static_cast<int>(tempos.size()) - 1; ++i)
        {
            lastTempo = i;
            const int ticks = tempos[i + 1].tick - tempos[i].tick;
            if (accTicks + ticks >= tick)
            {
                break;
            }
            accTicks += ticks;
            total += ticksToSec(ticks, beatTicks, tempos[i].bpm);
            lastTempo = i + 1;
        }

        total += ticksToSec(tick - tempos[lastTempo].tick, beatTicks, tempos[lastTempo].bpm);
        return total;
    }

    class SusParser
    {
    public:
        [[nodiscard]] SUS parseText(const std::string& text)
        {
            ticksPerBeat_ = kTicksPerBeatDefault;
            measureOffset_ = 0;
            waveOffset_ = 0.0f;
            title_.clear();
            artist_.clear();
            designer_.clear();
            bpmDefinitions_.clear();
            bars_.clear();

            SUS sus{};
            std::vector<SusDataLine> noteLines;
            std::vector<SusDataLine> bpmLines;

            std::stringstream stream(text);
            std::string rawLine;
            while (std::getline(stream, rawLine))
            {
                const std::string line = trim(rawLine);
                if (!startsWith(line, "#"))
                {
                    continue;
                }

                if (isCommand(line))
                {
                    processCommand(line);
                }
                else
                {
                    SusDataLine susLine(measureOffset_, line);
                    const std::string& header = susLine.header;
                    if (header.size() != 5 && header.size() != 6)
                    {
                        continue;
                    }

                    if (endsWith(header, "02") && isDigitString(header))
                    {
                        sus.barlengths.push_back({susLine.getEffectiveMeasure(), std::strtof(susLine.data.c_str(), nullptr)});
                    }
                    else if (startsWith(header, "BPM"))
                    {
                        bpmDefinitions_[header.substr(3)] = std::strtof(susLine.data.c_str(), nullptr);
                    }
                    else if (endsWith(header, "08"))
                    {
                        bpmLines.push_back(susLine);
                    }
                    else
                    {
                        noteLines.push_back(susLine);
                    }
                }
            }

            if (sus.barlengths.empty())
            {
                sus.barlengths.push_back({0, 4.0f});
            }

            bars_ = getBars(sus.barlengths);
            sus.bpms = getBpms(bpmLines);

            std::map<int, std::vector<SUSNote>> slideStreams;
            std::map<int, std::vector<SUSNote>> guideStreams;
            for (const auto& line : noteLines)
            {
                const std::string& header = line.header;
                if (header.size() == 5 && header[3] == '1')
                {
                    const auto append = getNotes(line);
                    sus.taps.insert(sus.taps.end(), append.begin(), append.end());
                }
                else if (header.size() == 5 && header[3] == '5')
                {
                    const auto append = getNotes(line);
                    sus.directionals.insert(sus.directionals.end(), append.begin(), append.end());
                }
                else if (header.size() == 6 && header[3] == '3')
                {
                    const int channel = static_cast<int>(std::strtoul(header.substr(5, 1).c_str(), nullptr, 36));
                    auto append = getNotes(line);
                    auto& streamRef = slideStreams[channel];
                    streamRef.insert(streamRef.end(), append.begin(), append.end());
                }
                else if (header.size() == 6 && header[3] == '9')
                {
                    const int channel = static_cast<int>(std::strtoul(header.substr(5, 1).c_str(), nullptr, 36));
                    auto append = getNotes(line);
                    auto& streamRef = guideStreams[channel];
                    streamRef.insert(streamRef.end(), append.begin(), append.end());
                }
            }

            for (const auto& [_, stream] : slideStreams)
            {
                const auto notes = getNoteStream(stream);
                sus.slides.insert(sus.slides.end(), notes.begin(), notes.end());
            }
            for (const auto& [_, stream] : guideStreams)
            {
                const auto notes = getNoteStream(stream);
                sus.guides.insert(sus.guides.end(), notes.begin(), notes.end());
            }

            sus.metadata["title"] = title_;
            sus.metadata["artist"] = artist_;
            sus.metadata["designer"] = designer_;
            sus.waveOffset = waveOffset_;
            return sus;
        }

    private:
        [[nodiscard]] bool isCommand(const std::string& line) const
        {
            if (line.size() < 2)
            {
                return false;
            }
            if (std::isdigit(static_cast<unsigned char>(line[1])) != 0)
            {
                return false;
            }
            if (line.find('"') != std::string::npos)
            {
                const auto parts = splitWhitespace(line);
                if (parts.size() < 2)
                {
                    return false;
                }
                if (parts[0].find(':') != std::string::npos)
                {
                    return false;
                }
                const size_t firstQuote = line.find('"');
                const size_t lastQuote = line.find_last_of('"');
                return firstQuote != lastQuote && lastQuote != std::string::npos;
            }
            return line.find(':') == std::string::npos;
        }

        [[nodiscard]] int getTicks(int measure, int index, int total) const
        {
            int barIndex = 0;
            int accBarTicks = 0;
            for (size_t i = 0; i < bars_.size(); ++i)
            {
                if (bars_[i].measure > measure)
                {
                    break;
                }
                barIndex = static_cast<int>(i);
                accBarTicks += bars_[i].ticks;
            }

            return accBarTicks
                + ((measure - bars_[barIndex].measure) * bars_[barIndex].ticksPerMeasure)
                + ((index * bars_[barIndex].ticksPerMeasure) / total);
        }

        [[nodiscard]] std::vector<std::vector<SUSNote>> getNoteStream(const std::vector<SUSNote>& stream) const
        {
            std::vector<SUSNote> sorted = stream;
            std::stable_sort(sorted.begin(), sorted.end(), [](const SUSNote& left, const SUSNote& right) {
                return left.tick < right.tick;
            });

            std::vector<std::vector<SUSNote>> result;
            std::vector<SUSNote> current;
            bool newSlide = true;
            for (const auto& note : sorted)
            {
                if (newSlide)
                {
                    current.clear();
                    newSlide = false;
                }
                current.push_back(note);
                if (note.type == 2)
                {
                    result.push_back(current);
                    newSlide = true;
                }
            }
            return result;
        }

        [[nodiscard]] std::vector<SUSNote> getNotes(const SusDataLine& line) const
        {
            std::vector<SUSNote> notes;
            for (size_t i = 0; i + 1 < line.data.size(); i += 2)
            {
                if (line.data[i] == '0' && line.data[i + 1] == '0')
                {
                    continue;
                }
                notes.push_back(SUSNote{
                    getTicks(line.getEffectiveMeasure(), static_cast<int>(i), static_cast<int>(line.data.size())),
                    static_cast<int>(std::strtoul(line.header.substr(4, 1).c_str(), nullptr, 36)),
                    static_cast<int>(std::strtoul(line.data.substr(i + 1, 1).c_str(), nullptr, 36)),
                    static_cast<int>(std::strtoul(line.data.substr(i, 1).c_str(), nullptr, 36)),
                });
            }
            return notes;
        }

        [[nodiscard]] std::vector<BPM> getBpms(const std::vector<SusDataLine>& lines) const
        {
            std::vector<BPM> bpms;
            for (const auto& line : lines)
            {
                for (size_t i = 0; i + 1 < line.data.size(); i += 2)
                {
                    if (line.data[i] == '0' && line.data[i + 1] == '0')
                    {
                        continue;
                    }

                    const int tick = getTicks(line.getEffectiveMeasure(), static_cast<int>(i), static_cast<int>(line.data.size()));
                    float bpm = 120.0f;
                    const std::string key = line.data.substr(i, 2);
                    auto it = bpmDefinitions_.find(key);
                    if (it != bpmDefinitions_.end())
                    {
                        bpm = it->second;
                    }
                    bpms.push_back({tick, bpm});
                }
            }

            std::sort(bpms.begin(), bpms.end(), [](const BPM& left, const BPM& right) {
                return left.tick < right.tick;
            });
            return bpms;
        }

        [[nodiscard]] std::vector<Bar> getBars(const std::vector<BarLength>& lengths) const
        {
            std::vector<Bar> bars;
            bars.reserve(lengths.size());
            bars.push_back({lengths[0].bar, static_cast<int>(lengths[0].length * ticksPerBeat_), 0});
            for (size_t i = 1; i < lengths.size(); ++i)
            {
                const int measure = lengths[i].bar;
                const int ticksPerMeasure = static_cast<int>(lengths[i].length * ticksPerBeat_);
                const int ticks = static_cast<int>((measure - lengths[i - 1].bar) * lengths[i - 1].length * ticksPerBeat_);
                bars.push_back({measure, ticksPerMeasure, ticks});
            }

            std::sort(bars.begin(), bars.end(), [](const Bar& left, const Bar& right) {
                return left.measure < right.measure;
            });
            return bars;
        }

        void processCommand(const std::string& line)
        {
            const size_t keyPos = line.find(' ');
            if (keyPos == std::string::npos)
            {
                return;
            }

            std::string key = line.substr(1, keyPos - 1);
            std::string value = line.substr(keyPos + 1);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

            if (startsWith(value, "\"") && endsWith(value, "\""))
            {
                value = value.substr(1, value.size() - 2);
            }

            if (key == "TITLE")
            {
                title_ = value;
            }
            else if (key == "ARTIST")
            {
                artist_ = value;
            }
            else if (key == "DESIGNER")
            {
                designer_ = value;
            }
            else if (key == "WAVEOFFSET")
            {
                waveOffset_ = std::strtof(value.c_str(), nullptr);
            }
            else if (key == "MEASUREBS")
            {
                measureOffset_ = std::atoi(value.c_str());
            }
            else if (key == "REQUEST")
            {
                const auto requestArgs = splitWhitespace(value);
                if (requestArgs.size() == 2 && requestArgs[0] == "ticks_per_beat")
                {
                    ticksPerBeat_ = std::atoi(requestArgs[1].c_str());
                }
            }
        }

        int ticksPerBeat_{kTicksPerBeatDefault};
        int measureOffset_{};
        float waveOffset_{};
        std::string title_;
        std::string artist_;
        std::string designer_;
        std::map<std::string, float> bpmDefinitions_;
        std::vector<Bar> bars_;
    };

    void sortHoldSteps(const Score& score, HoldNote& hold)
    {
        std::stable_sort(hold.steps.begin(), hold.steps.end(), [&score](const HoldStep& lhs, const HoldStep& rhs) {
            const auto& left = score.notes.at(lhs.ID);
            const auto& right = score.notes.at(rhs.ID);
            return left.tick == right.tick ? left.lane < right.lane : left.tick < right.tick;
        });
    }

    Score susToScore(const SUS& sus, float normalizedOffsetMs)
    {
        int nextID = 1;

        Score score{};
        score.musicOffsetMs = normalizedOffsetMs;

        std::unordered_map<std::string, FlickType> flicks;
        std::unordered_set<std::string> criticals;
        std::unordered_set<std::string> stepIgnore;
        std::unordered_set<std::string> easeIns;
        std::unordered_set<std::string> easeOuts;
        std::unordered_set<std::string> slideKeys;
        std::unordered_set<std::string> frictions;
        std::unordered_set<std::string> hiddenHolds;

        for (const auto& slide : sus.slides)
        {
            for (const auto& note : slide)
            {
                if (note.type == 1 || note.type == 2 || note.type == 3 || note.type == 5)
                {
                    slideKeys.insert(noteKey(note));
                }
            }
        }

        for (const auto& dir : sus.directionals)
        {
            const std::string key = noteKey(dir);
            switch (dir.type)
            {
                case 1:
                    flicks.insert_or_assign(key, FlickType::Default);
                    break;
                case 3:
                    flicks.insert_or_assign(key, FlickType::Left);
                    break;
                case 4:
                    flicks.insert_or_assign(key, FlickType::Right);
                    break;
                case 2:
                    easeIns.insert(key);
                    break;
                case 5:
                case 6:
                    easeOuts.insert(key);
                    break;
                default:
                    break;
            }
        }

        for (const auto& tap : sus.taps)
        {
            const std::string key = noteKey(tap);
            switch (tap.type)
            {
                case 2:
                    criticals.insert(key);
                    break;
                case 3:
                    stepIgnore.insert(key);
                    break;
                case 5:
                    frictions.insert(key);
                    break;
                case 6:
                    criticals.insert(key);
                    frictions.insert(key);
                    break;
                case 7:
                    hiddenHolds.insert(key);
                    break;
                case 8:
                    hiddenHolds.insert(key);
                    criticals.insert(key);
                    break;
                default:
                    break;
            }
        }

        for (const auto& tap : sus.taps)
        {
            if (tap.type == 7 || tap.type == 8)
            {
                continue;
            }
            if (tap.lane - 2 < kMinLane || tap.lane - 2 > kMaxLane)
            {
                continue;
            }

            const std::string key = noteKey(tap);
            if (slideKeys.contains(key))
            {
                continue;
            }

            Note note{};
            note.type = NoteType::Tap;
            note.tick = tap.tick;
            note.lane = tap.lane - 2;
            note.width = tap.width;
            note.critical = criticals.contains(key);
            note.friction = frictions.contains(key);
            auto flickIt = flicks.find(key);
            note.flick = flickIt == flicks.end() ? FlickType::None : flickIt->second;
            note.ID = nextID++;
            score.notes[note.ID] = note;
        }

        const auto fillSlides = [&](const std::vector<std::vector<SUSNote>>& slides, bool isGuide)
        {
            for (const auto& slide : slides)
            {
                if (slide.size() < 2)
                {
                    continue;
                }

                auto start = std::find_if(slide.begin(), slide.end(), [](const SUSNote& note) {
                    return note.type == 1 || note.type == 2;
                });
                if (start == slide.end())
                {
                    continue;
                }

                const std::string criticalKey = noteKey(slide[0]);
                const bool critical = criticals.contains(criticalKey);

                HoldNote hold{};
                const int startID = nextID++;
                hold.steps.reserve(slide.size() - 2);

                for (const auto& susNote : slide)
                {
                    const std::string key = noteKey(susNote);
                    EaseType ease = EaseType::Linear;
                    if (easeIns.contains(key))
                    {
                        ease = EaseType::EaseIn;
                    }
                    else if (easeOuts.contains(key))
                    {
                        ease = EaseType::EaseOut;
                    }

                    switch (susNote.type)
                    {
                        case 1:
                        {
                            Note note{};
                            note.type = NoteType::Hold;
                            note.tick = susNote.tick;
                            note.lane = susNote.lane - 2;
                            note.width = susNote.width;
                            note.critical = critical;
                            note.ID = startID;
                            if (isGuide)
                            {
                                hold.startType = HoldNoteType::Guide;
                            }
                            else
                            {
                                note.friction = frictions.contains(key);
                                hold.startType = hiddenHolds.contains(key) ? HoldNoteType::Hidden : HoldNoteType::Normal;
                            }
                            score.notes[note.ID] = note;
                            hold.start = HoldStep{note.ID, HoldStepType::Normal, ease};
                            break;
                        }
                        case 2:
                        {
                            Note note{};
                            note.type = NoteType::HoldEnd;
                            note.tick = susNote.tick;
                            note.lane = susNote.lane - 2;
                            note.width = susNote.width;
                            note.critical = critical ? true : criticals.contains(key);
                            note.ID = nextID++;
                            note.parentID = startID;
                            if (isGuide)
                            {
                                hold.endType = HoldNoteType::Guide;
                            }
                            else
                            {
                                auto flickIt = flicks.find(key);
                                note.flick = flickIt == flicks.end() ? FlickType::None : flickIt->second;
                                note.friction = frictions.contains(key);
                                hold.endType = hiddenHolds.contains(key) ? HoldNoteType::Hidden : HoldNoteType::Normal;
                            }
                            score.notes[note.ID] = note;
                            hold.end = note.ID;
                            break;
                        }
                        case 3:
                        case 5:
                        {
                            Note note{};
                            note.type = NoteType::HoldMid;
                            note.tick = susNote.tick;
                            note.lane = susNote.lane - 2;
                            note.width = susNote.width;
                            note.critical = critical;
                            note.ID = nextID++;
                            note.parentID = startID;
                            HoldStepType type = susNote.type == 3 ? HoldStepType::Normal : HoldStepType::Hidden;
                            if (stepIgnore.contains(key))
                            {
                                type = HoldStepType::Skip;
                            }
                            score.notes[note.ID] = note;
                            hold.steps.push_back(HoldStep{note.ID, type, ease});
                            break;
                        }
                        default:
                            break;
                    }
                }

                if (hold.start.ID == 0 || hold.end == 0)
                {
                    throw std::runtime_error("Invalid hold note");
                }

                sortHoldSteps(score, hold);
                score.holdNotes[startID] = hold;
            }
        };

        fillSlides(sus.slides, false);
        fillSlides(sus.guides, true);

        score.tempoChanges.reserve(sus.bpms.size());
        for (const auto& bpm : sus.bpms)
        {
            score.tempoChanges.push_back({bpm.tick, bpm.bpm});
        }
        if (score.tempoChanges.empty())
        {
            score.tempoChanges.push_back({0, 120.0f});
        }
        std::sort(score.tempoChanges.begin(), score.tempoChanges.end(), [](const Tempo& lhs, const Tempo& rhs) {
            return lhs.tick < rhs.tick;
        });

        return score;
    }

    float getNoteCenter(const Note& note)
    {
        return static_cast<float>(note.lane - 6) + static_cast<float>(note.width) / 2.0f;
    }

    std::vector<HitEvent> calculateHitEvents(const Score& score)
    {
        std::vector<HitEvent> hitEvents;
        std::unordered_map<int, HoldStepType> holdStepTypesById;
        holdStepTypesById.reserve(score.notes.size());
        for (const auto& [holdId, hold] : score.holdNotes)
        {
            (void)holdId;
            for (const auto& step : hold.steps)
            {
                holdStepTypesById.emplace(step.ID, step.type);
            }
        }

        for (const auto& [id, note] : score.notes)
        {
            (void)id;
            std::string kind = "tap";
            bool playEvent = true;

            if (note.type == NoteType::Hold)
            {
                const HoldNote& hold = score.holdNotes.at(note.ID);
                playEvent = hold.startType == HoldNoteType::Normal;
            }
            else if (note.type == NoteType::HoldEnd)
            {
                const HoldNote& hold = score.holdNotes.at(note.parentID);
                playEvent = hold.endType == HoldNoteType::Normal;
            }

            if (playEvent && note.type == NoteType::HoldMid)
            {
                auto stepTypeIt = holdStepTypesById.find(note.ID);
                if (stepTypeIt != holdStepTypesById.end() && stepTypeIt->second == HoldStepType::Hidden)
                {
                    playEvent = false;
                }
                else
                {
                    kind = "tick";
                }
            }
            else if (note.isFlick())
            {
                kind = note.critical ? "flickCritical" : "flick";
            }
            else if (note.friction)
            {
                kind = note.critical ? "traceCritical" : "trace";
            }
            else if (note.critical && note.type == NoteType::Tap)
            {
                kind = "criticalTap";
            }
            else
            {
                kind = "tap";
            }

            if (!playEvent)
            {
                continue;
            }

            hitEvents.push_back(HitEvent{
                accumulateDuration(note.tick, kTicksPerBeatDefault, score.tempoChanges),
                -1.0,
                getNoteCenter(note),
                static_cast<float>(note.width),
                kind,
                note.critical,
            });

            if (note.type == NoteType::Hold)
            {
                const HoldNote& hold = score.holdNotes.at(note.ID);
                if (!hold.isGuide() && hold.startType == HoldNoteType::Normal)
                {
                    const Note& endNote = score.notes.at(hold.end);
                    hitEvents.push_back(HitEvent{
                        accumulateDuration(note.tick, kTicksPerBeatDefault, score.tempoChanges),
                        accumulateDuration(endNote.tick, kTicksPerBeatDefault, score.tempoChanges),
                        getNoteCenter(note),
                        static_cast<float>(note.width),
                        note.critical ? "holdLoopCritical" : "holdLoop",
                        note.critical,
                    });
                }
            }
        }

        std::stable_sort(hitEvents.begin(), hitEvents.end(), [](const HitEvent& lhs, const HitEvent& rhs) {
            if (lhs.timeSec == rhs.timeSec)
            {
                return lhs.center < rhs.center;
            }
            return lhs.timeSec < rhs.timeSec;
        });

        return hitEvents;
    }

    std::string escapeShell(const std::string& value)
    {
#if defined(_WIN32)
        std::string escaped = "\"";
        for (char c : value)
        {
            if (c == '"')
            {
                escaped += "\\\"";
            }
            else
            {
                escaped += c;
            }
        }
        escaped += "\"";
        return escaped;
#else
        std::string escaped = "'";
        for (char c : value)
        {
            if (c == '\'')
            {
                escaped += "'\\''";
            }
            else
            {
                escaped += c;
            }
        }
        escaped += "'";
        return escaped;
#endif
    }

    int runCommand(const std::string& command)
    {
        const int status = std::system(command.c_str());
        if (status == -1)
        {
            throw std::runtime_error("Failed to start process: " + command);
        }
#if defined(_WIN32)
        if (status != 0)
        {
            throw std::runtime_error("Process failed: " + command);
        }
        return status;
#else
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            throw std::runtime_error("Process failed: " + command);
        }
        return WEXITSTATUS(status);
#endif
    }

    std::vector<float> readFloatFile(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("Failed to open decoded audio: " + path.string());
        }
        stream.seekg(0, std::ios::end);
        const std::streamsize size = stream.tellg();
        stream.seekg(0, std::ios::beg);
        if (size % static_cast<std::streamsize>(sizeof(float)) != 0)
        {
            throw std::runtime_error("Decoded audio size is not aligned to float samples");
        }
        std::vector<float> data(static_cast<size_t>(size / static_cast<std::streamsize>(sizeof(float))));
        if (!stream.read(reinterpret_cast<char*>(data.data()), size))
        {
            throw std::runtime_error("Failed to read decoded audio data");
        }
        return data;
    }

    fs::path createTempPath(std::string_view suffix)
    {
#if defined(_WIN32)
        const fs::path tempDir = fs::temp_directory_path();
        const auto seed = static_cast<long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            const fs::path dir = tempDir / ("sus-key-audio-" + std::to_string(seed) + "-" + std::to_string(attempt));
            if (fs::create_directory(dir))
            {
                return dir / suffix;
            }
        }
        throw std::runtime_error("Failed to create temporary directory");
#else
        std::string pattern = (fs::temp_directory_path() / fs::path("sus-key-audio-XXXXXX")).string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* dir = mkdtemp(buffer.data());
        if (dir == nullptr)
        {
            throw std::runtime_error("mkdtemp failed");
        }
        return fs::path(dir) / suffix;
#endif
    }

    std::vector<float> decodeSoundToMono(const fs::path& inputPath)
    {
        const fs::path tempPath = createTempPath("decoded.f32");
        const std::string command =
            "ffmpeg -v error -y -i " + escapeShell(inputPath.string()) +
            " -f f32le -acodec pcm_f32le -ac 1 -ar " + std::to_string(kTargetSampleRate) +
            " " + escapeShell(tempPath.string());
        runCommand(command);
        std::vector<float> decoded = readFloatFile(tempPath);
        fs::remove_all(tempPath.parent_path());
        return decoded;
    }

    void mixOneShot(std::vector<float>& target, size_t startFrame, const std::vector<float>& source, float volume)
    {
        if (startFrame >= target.size())
        {
            return;
        }
        for (size_t i = 0; i < source.size() && startFrame + i < target.size(); ++i)
        {
            target[startFrame + i] += source[i] * volume;
        }
    }

    void mixHoldLoop(std::vector<float>& target, size_t startFrame, size_t endFrame, const std::vector<float>& source, float volume)
    {
        if (startFrame >= target.size() || endFrame <= startFrame || source.empty())
        {
            return;
        }

        const size_t safeEndFrame = std::min(endFrame, target.size());
        const size_t introFrames = std::min<size_t>(3000, source.size());
        const size_t loopStart = std::min<size_t>(3000, source.size());
        const size_t loopEnd = std::max(loopStart + 1, source.size() > 3000 ? source.size() - 3000 : source.size());
        const size_t loopLength = std::max<size_t>(1, loopEnd - loopStart);

        size_t cursor = startFrame;
        size_t sourceIndex = 0;
        while (cursor < safeEndFrame && sourceIndex < introFrames)
        {
            target[cursor++] += source[sourceIndex++] * volume;
        }

        while (cursor < safeEndFrame)
        {
            const size_t loopIndex = loopStart + ((cursor - startFrame - introFrames) % loopLength);
            target[cursor++] += source[loopIndex] * volume;
        }
    }

    void clampPcm(std::vector<float>& pcm)
    {
        for (float& sample : pcm)
        {
            sample = std::clamp(sample, -1.0f, 1.0f);
        }
    }

    void writeRawFloatFile(const fs::path& path, const std::vector<float>& data)
    {
        std::ofstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("Failed to open raw output file");
        }
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
        if (!stream)
        {
            throw std::runtime_error("Failed to write raw output file");
        }
    }

    void encodeOutput(const std::vector<float>& mix, const fs::path& outPath, const std::string& format)
    {
        const fs::path tempPath = createTempPath("mix.f32");
        writeRawFloatFile(tempPath, mix);

        std::string extension = format;
        if (extension.empty())
        {
            extension = outPath.extension().string();
            if (!extension.empty() && extension[0] == '.')
            {
                extension.erase(extension.begin());
            }
        }
        if (extension.empty())
        {
            extension = "mp3";
        }

        std::string command =
            "ffmpeg -v error -y -f f32le -ar " + std::to_string(kTargetSampleRate) +
            " -ac 1 -i " + escapeShell(tempPath.string());
        if (extension == "wav")
        {
            command += " -ac 2 -c:a pcm_s16le " + escapeShell(outPath.string());
        }
        else
        {
            command += " -ac 2 -c:a libmp3lame -b:a 128k " + escapeShell(outPath.string());
        }

        runCommand(command);
        fs::remove_all(tempPath.parent_path());
    }

    std::string readTextFile(const fs::path& path)
    {
        std::ifstream stream(path);
        if (!stream)
        {
            throw std::runtime_error("Failed to open SUS file: " + path.string());
        }
        std::stringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

    double normalizeOffsetMs(const std::optional<double>& rawOffsetMs, const std::string& susText)
    {
        if (rawOffsetMs.has_value())
        {
            return -rawOffsetMs.value();
        }

        const std::string key = "#WAVEOFFSET";
        std::stringstream stream(susText);
        std::string line;
        while (std::getline(stream, line))
        {
            const std::string trimmed = trim(line);
            if (!startsWith(trimmed, key))
            {
                continue;
            }
            const auto parts = splitWhitespace(trimmed);
            if (parts.size() < 2)
            {
                return 0.0;
            }
            return std::strtod(parts[1].c_str(), nullptr) * 1000.0;
        }
        return 0.0;
    }

    std::string resolveSoundKey(const HitEvent& event)
    {
        return event.kind;
    }

    fs::path resolveDefaultSoundRoot(const char* argv0)
    {
        std::error_code error;
        if (argv0 != nullptr && std::string_view(argv0).size() > 0)
        {
            const fs::path executablePath = fs::absolute(fs::path(argv0), error);
            if (!error)
            {
                const fs::path candidate = executablePath.parent_path().parent_path() / "assets" / "sound";
                if (fs::exists(candidate, error) && fs::is_directory(candidate, error))
                {
                    return candidate;
                }
            }
        }

        error.clear();
        const fs::path cwdCandidate = fs::current_path(error) / "assets" / "sound";
        if (!error && fs::exists(cwdCandidate, error) && fs::is_directory(cwdCandidate, error))
        {
            return cwdCandidate;
        }

        return fs::path("assets") / "sound";
    }

    CliOptions parseArgs(int argc, char** argv)
    {
        CliOptions options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string token = argv[i];
            const auto nextValue = [&](const char* flag) -> std::string
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error(std::string("Missing value for ") + flag);
                }
                return argv[++i];
            };

            if (token == "--sus")
            {
                options.susPath = nextValue("--sus");
            }
            else if (token == "--out")
            {
                options.outPath = nextValue("--out");
            }
            else if (token == "--offset")
            {
                options.offsetMs = std::strtod(nextValue("--offset").c_str(), nullptr);
            }
            else if (token == "--format")
            {
                options.format = nextValue("--format");
            }
            else if (token == "--sound-root")
            {
                options.soundRoot = nextValue("--sound-root");
            }
            else if (token == "--help" || token == "-h")
            {
                std::cout << "Usage: render-key-audio --sus <chart.sus> --out <output.mp3> [--offset <ms>] [--format mp3|wav] [--sound-root <dir>]\n";
                std::cout << "Default sound-root: ./assets/sound (or ../assets/sound beside binary)\n";
                std::exit(0);
            }
        }

        if (options.susPath.empty() || options.outPath.empty())
        {
            throw std::runtime_error("Missing required --sus or --out");
        }

        if (options.soundRoot.empty())
        {
            options.soundRoot = resolveDefaultSoundRoot(argc > 0 ? argv[0] : nullptr);
        }

        return options;
    }
}

int main(int argc, char** argv)
{
    try
    {
        const CliOptions options = parseArgs(argc, argv);
        const std::string susText = readTextFile(options.susPath);
        const double normalizedOffsetMs = normalizeOffsetMs(options.offsetMs, susText);

        SusParser parser;
        const SUS sus = parser.parseText(susText);
        const Score score = susToScore(sus, static_cast<float>(normalizedOffsetMs));
        std::vector<HitEvent> hitEvents = calculateHitEvents(score);
        for (HitEvent& event : hitEvents)
        {
            event.timeSec -= normalizedOffsetMs / 1000.0;
            if (event.endTimeSec >= 0.0)
            {
                event.endTimeSec -= normalizedOffsetMs / 1000.0;
            }
        }

        std::unordered_map<std::string, std::vector<float>> soundBuffers;
        for (const HitEvent& event : hitEvents)
        {
            const std::string key = resolveSoundKey(event);
            if (soundBuffers.contains(key))
            {
                continue;
            }
            const auto soundIt = kSoundDefinitions.find(key);
            if (soundIt == kSoundDefinitions.end())
            {
                throw std::runtime_error("Missing sound definition for " + key);
            }
            soundBuffers[key] = decodeSoundToMono(options.soundRoot / soundIt->second.file);
        }

        double maxTimeSec = 1.0;
        for (const HitEvent& event : hitEvents)
        {
            const auto& soundBuffer = soundBuffers.at(resolveSoundKey(event));
            if (event.endTimeSec >= 0.0)
            {
                maxTimeSec = std::max(maxTimeSec, event.endTimeSec);
            }
            else
            {
                maxTimeSec = std::max(maxTimeSec, event.timeSec + static_cast<double>(soundBuffer.size()) / kTargetSampleRate);
            }
        }

        std::vector<float> mix(static_cast<size_t>(std::ceil((maxTimeSec + 1.0) * kTargetSampleRate)), 0.0f);
        for (const HitEvent& event : hitEvents)
        {
            const std::string key = resolveSoundKey(event);
            const auto soundDef = kSoundDefinitions.at(key);
            const auto& soundBuffer = soundBuffers.at(key);
            const size_t startFrame = event.timeSec > 0.0 ? static_cast<size_t>(std::llround(event.timeSec * kTargetSampleRate)) : 0;
            if (soundDef.loop && event.endTimeSec > event.timeSec)
            {
                const size_t endFrame = static_cast<size_t>(std::llround(event.endTimeSec * kTargetSampleRate));
                mixHoldLoop(mix, startFrame, endFrame, soundBuffer, soundDef.volume);
            }
            else
            {
                mixOneShot(mix, startFrame, soundBuffer, soundDef.volume);
            }
        }

        clampPcm(mix);
        encodeOutput(mix, options.outPath, options.format);

        std::cout << "Wrote key audio: " << options.outPath << "\n";
        std::cout << "Events: " << hitEvents.size() << "\n";
        std::cout << "Offset applied: " << static_cast<long long>(std::llround(normalizedOffsetMs)) << " ms\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
