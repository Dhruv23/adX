// Phase 6's command constructors, addressed by id (phase_6.md §3 rule 3: every mutation
// is a command).
//
// Phase 2's constructors address channels and patterns by name, which is right for a
// script and wrong for a panel: a panel holds an id across a rename. These take ids, and
// like every PendingCommand they are built against the project only when executed.
//
// A gesture that needs two commands - a sample dropped on the playlist is a pool entry
// and a placement - is a method on the Project that groups them, so it is still one
// undo step ("drop is one command, so it is one undo step", §4.6).

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/PendingCommand.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/preset/Preset.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/TypeCatalog.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace py = pybind11;
namespace project = adx::project;
namespace core = adx::core;

using adx::bindings::parseColorText;
using adx::bindings::PendingCommand;

namespace {

using Build = std::function<std::unique_ptr<project::Command>(const project::Project&)>;

PendingCommand made(std::string description, Build build) {
    return PendingCommand{.build = std::move(build), .description = std::move(description)};
}

template<class T, class... Args> std::unique_ptr<project::Command> make(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

using PointsArray = py::array_t<double, py::array::c_style | py::array::forcecast>;

/// Breakpoints from an (n, 2..8) float64 array: tick, value, then optionally curve kind,
/// tension, c1x, c1y, c2x, c2y. Sorted by tick, as the model keeps them.
std::vector<project::Breakpoint> breakpointsOf(const PointsArray& points) {
    std::vector<project::Breakpoint> out;
    if (points.ndim() != 2 || points.shape(1) < 2) {
        if (points.size() == 0) {
            return out;
        }
        throw py::value_error("points must be an (n, 2..8) array");
    }
    const auto rows = points.unchecked<2>();
    const py::ssize_t columns = points.shape(1);
    for (py::ssize_t i = 0; i < rows.shape(0); ++i) {
        project::Breakpoint point;
        point.at = core::Ticks{static_cast<std::int64_t>(std::max(0.0, rows(i, 0)))};
        point.value = static_cast<float>(rows(i, 1));
        if (columns > 2) {
            point.curve.kind = static_cast<core::CurveKind>(std::clamp(
                static_cast<int>(rows(i, 2)), 0, static_cast<int>(core::kCurveKindCount) - 1));
        }
        if (columns > 3) {
            point.curve.tension = static_cast<float>(std::clamp(rows(i, 3), -1.0, 1.0));
        }
        if (columns > 7) {
            point.curve.c1x = static_cast<float>(rows(i, 4));
            point.curve.c1y = static_cast<float>(rows(i, 5));
            point.curve.c2x = static_cast<float>(rows(i, 6));
            point.curve.c2y = static_cast<float>(rows(i, 7));
        }
        out.push_back(point);
    }
    std::ranges::stable_sort(out, {}, &project::Breakpoint::at);
    return out;
}

project::ParamRef resolveOrThrow(const project::Project& p, const std::string& path) {
    const project::ParamResolution resolved = project::ParamRegistry::resolve(path, p);
    if (!resolved.ok()) {
        throw std::invalid_argument("no such parameter: " + path);
    }
    return resolved.ref;
}

project::Controller controllerFrom(const project::Project& p, const py::dict& fields,
                                   project::Controller base) {
    const auto has = [&fields](const char* key) { return fields.contains(key); };
    if (has("name")) {
        base.name = fields["name"].cast<std::string>();
    }
    if (has("kind") &&
        !project::controllerKindFromString(fields["kind"].cast<std::string>(), base.kind)) {
        throw std::invalid_argument("unknown controller kind");
    }
    if (has("target")) {
        base.targetPath = fields["target"].cast<std::string>();
        base.target = resolveOrThrow(p, base.targetPath);
    }
    if (has("shape") &&
        !project::lfoShapeFromString(fields["shape"].cast<std::string>(), base.shape)) {
        throw std::invalid_argument("unknown LFO shape");
    }
    if (has("period")) {
        base.period = core::Ticks{std::max<std::int64_t>(1, fields["period"].cast<std::int64_t>())};
    }
    if (has("source_channel")) {
        base.sourceChannel = core::ChannelId{fields["source_channel"].cast<std::uint32_t>()};
    }
    if (has("source_insert")) {
        base.sourceInsert = core::InsertId{fields["source_insert"].cast<std::uint32_t>()};
    }
    const std::array<std::pair<const char*, float*>, 8> numbers{{
        {"depth", &base.depth},
        {"center", &base.center},
        {"phase", &base.phase},
        {"attack", &base.attackMs},
        {"decay", &base.decayMs},
        {"sustain", &base.sustain},
        {"release", &base.releaseMs},
        {"gain", &base.gainDb},
    }};
    for (const auto& [key, field] : numbers) {
        if (has(key)) {
            *field = fields[key].cast<float>();
        }
    }
    return base;
}

// NOLINTBEGIN(bugprone-exception-escape) - a builder throws std::invalid_argument for an
// id or path that names nothing, which pybind11 turns into the ValueError the caller sees.

void channelCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "AddChannelNew",
        [](const std::string& name, const std::string& instrument, std::uint32_t output,
           const std::string& color) {
            return made("AddChannel", [=](const project::Project& p) {
                project::Channel prototype;
                prototype.name = name;
                prototype.instrument.type = instrument;
                prototype.output = output != 0 ? core::InsertId{output} : p.mixer.master;
                prototype.color = parseColorText(color, prototype.color);
                return make<project::AddChannel>(std::move(prototype));
            });
        },
        py::arg("name"), py::arg("instrument") = "additive", py::arg("output") = 0,
        py::arg("color") = "");
    // GIL: trivial - builds a closure
    c.def(
        "RemoveChannelId",
        [](std::uint32_t channel) {
            return made("RemoveChannel", [=](const project::Project&) {
                return make<project::RemoveChannel>(core::ChannelId{channel});
            });
        },
        py::arg("channel"));
    // GIL: trivial - builds a closure
    c.def(
        "SetChannelField",
        [](std::uint32_t channel, const std::string& field, double value) {
            static const std::array<std::pair<const char*, project::ChannelField>, 7> kFields{{
                {"volume", project::ChannelField::Volume},
                {"pan", project::ChannelField::Pan},
                {"pitch", project::ChannelField::PitchCents},
                {"mute", project::ChannelField::Muted},
                {"solo", project::ChannelField::Soloed},
                {"polyphony", project::ChannelField::MaxPolyphony},
                {"steal", project::ChannelField::StealMode},
            }};
            const auto match = std::ranges::find(
                kFields, field, [](const auto& entry) { return std::string_view(entry.first); });
            if (match == kFields.end()) {
                throw std::invalid_argument("unknown channel field " + field);
            }
            const project::ChannelField which = match->second;
            return made("SetChannelValue", [=](const project::Project&) {
                return make<project::SetChannelValue>(core::ChannelId{channel}, which, value);
            });
        },
        py::arg("channel"), py::arg("field"), py::arg("value"),
        "field: volume, pan, pitch, mute, solo, polyphony or steal (as its enum index).");
    // GIL: trivial - builds a closure
    c.def(
        "RenameChannelId",
        [](std::uint32_t channel, const std::string& name) {
            return made("RenameChannel", [=](const project::Project&) {
                return make<project::RenameChannel>(core::ChannelId{channel}, name);
            });
        },
        py::arg("channel"), py::arg("name"));
    // GIL: trivial - builds a closure
    c.def(
        "SetChannelColor",
        [](std::uint32_t channel, const std::string& color) {
            return made("SetChannelColor", [=](const project::Project&) {
                return make<project::SetChannelColor>(core::ChannelId{channel},
                                                      parseColorText(color, project::Color{}));
            });
        },
        py::arg("channel"), py::arg("color"));
    // GIL: trivial - builds a closure
    c.def(
        "SetChannelOutput",
        [](std::uint32_t channel, std::uint32_t insert) {
            return made("SetChannelOutput", [=](const project::Project&) {
                return make<project::SetChannelOutput>(core::ChannelId{channel},
                                                       core::InsertId{insert});
            });
        },
        py::arg("channel"), py::arg("insert"));
}

/// What a channel plays: its instrument, its parameters and their curves, its sampler
/// zones and its arpeggiator.
void instrumentCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "SetInstrumentType",
        [](std::uint32_t channel, const std::string& type) {
            return made("SetInstrumentType", [=](const project::Project&) {
                return make<project::SetInstrumentType>(core::ChannelId{channel}, type);
            });
        },
        py::arg("channel"), py::arg("type"));
    // GIL: trivial - builds a closure
    c.def(
        "SetChannelParam",
        [](std::uint32_t channel, const std::string& name, double value) {
            return made("SetChannelParam", [=](const project::Project&) {
                return make<project::SetChannelParam>(core::ChannelId{channel}, name, value);
            });
        },
        py::arg("channel"), py::arg("name"), py::arg("value"));
    // GIL: trivial - builds a closure
    c.def(
        "SetChannelParamCurve",
        [](std::uint32_t channel, const std::string& name, double value, int kind, float tension) {
            const core::Curve curve{.kind = static_cast<core::CurveKind>(std::clamp(
                                        kind, 0, static_cast<int>(core::kCurveKindCount) - 1)),
                                    .tension = tension};
            return made("SetChannelParam", [=](const project::Project&) {
                return make<project::SetChannelParam>(core::ChannelId{channel}, name, value, curve);
            });
        },
        py::arg("channel"), py::arg("name"), py::arg("value"), py::arg("kind"),
        py::arg("tension") = 0.0F, "A parameter with a curve: an envelope stage's shape.");
    // GIL: trivial - builds a closure over the zone list
    c.def(
        "SetChannelZones",
        [](std::uint32_t channel, const py::list& zones) {
            std::vector<project::SampleZone> parsed;
            for (const py::handle entry : zones) {
                const auto d = entry.cast<py::dict>();
                const auto get = [&d](const char* key, int fallback) {
                    return d.contains(key) ? d[key].cast<int>() : fallback;
                };
                project::SampleZone zone;
                zone.sample = core::SampleId{static_cast<std::uint32_t>(get("sample_id", 0))};
                zone.keyLow = static_cast<std::uint8_t>(std::clamp(get("key_low", 0), 0, 127));
                zone.keyHigh = static_cast<std::uint8_t>(std::clamp(get("key_high", 127), 0, 127));
                zone.rootKey = static_cast<std::uint8_t>(std::clamp(get("root", 60), 0, 127));
                zone.velocityLow = static_cast<std::uint8_t>(std::clamp(get("vel_low", 1), 1, 127));
                zone.velocityHigh =
                    static_cast<std::uint8_t>(std::clamp(get("vel_high", 127), 1, 127));
                zone.start = static_cast<std::uint32_t>(std::max(0, get("start", 0)));
                zone.end = static_cast<std::uint32_t>(std::max(0, get("end", 0)));
                zone.loop = static_cast<project::LoopMode>(std::clamp(get("loop", 0), 0, 4));
                zone.loopStart = static_cast<std::uint32_t>(std::max(0, get("loop_start", 0)));
                zone.loopEnd = static_cast<std::uint32_t>(std::max(0, get("loop_end", 0)));
                parsed.push_back(zone);
            }
            return made("SetChannelZones", [=](const project::Project&) {
                return make<project::SetChannelZones>(core::ChannelId{channel}, parsed);
            });
        },
        py::arg("channel"), py::arg("zones"),
        "Replace a sampler channel's zones with a list of dicts (Project.zones' keys).");
    // GIL: trivial - builds a closure
    c.def(
        "SetChannelArp",
        [](std::uint32_t channel, const std::string& mode, std::int64_t numerator,
           std::int64_t denominator, std::uint16_t octaves, float gate) {
            project::ArpSettings arp;
            if (!project::arpModeFromString(mode, arp.mode)) {
                throw std::invalid_argument("unknown arp mode " + mode);
            }
            arp.rate = core::Rational{.numerator = numerator,
                                      .denominator = std::max<std::int64_t>(1, denominator)};
            arp.octaves = std::max<std::uint16_t>(1, octaves);
            arp.gate = gate;
            return made("SetChannelArp", [=](const project::Project&) {
                return make<project::SetChannelArp>(core::ChannelId{channel}, arp);
            });
        },
        py::arg("channel"), py::arg("mode"), py::arg("numerator") = 1, py::arg("denominator") = 16,
        py::arg("octaves") = 1, py::arg("gate") = 0.8F);
}

void patternCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "AddPatternNew",
        [](const std::string& name, std::int64_t length, const std::string& color) {
            return made("AddPattern", [=](const project::Project&) {
                project::Pattern prototype;
                prototype.name = name;
                prototype.length = core::Ticks{std::max<std::int64_t>(1, length)};
                prototype.color = parseColorText(color, prototype.color);
                return make<project::AddPattern>(std::move(prototype));
            });
        },
        py::arg("name"), py::arg("length") = project::kDefaultPatternLength.value,
        py::arg("color") = "");
    // GIL: trivial - builds a closure
    c.def(
        "RemovePatternId",
        [](std::uint32_t pattern) {
            return made("RemovePattern", [=](const project::Project&) {
                return make<project::RemovePattern>(core::PatternId{pattern});
            });
        },
        py::arg("pattern"));
    // GIL: trivial - builds a closure
    c.def(
        "RenamePatternId",
        [](std::uint32_t pattern, const std::string& name) {
            return made("RenamePattern", [=](const project::Project&) {
                return make<project::RenamePattern>(core::PatternId{pattern}, name);
            });
        },
        py::arg("pattern"), py::arg("name"));
    // GIL: trivial - builds a closure
    c.def(
        "SetPatternLength",
        [](std::uint32_t pattern, std::int64_t length) {
            return made("SetPatternLength", [=](const project::Project&) {
                return make<project::SetPatternLength>(
                    core::PatternId{pattern}, core::Ticks{std::max<std::int64_t>(1, length)});
            });
        },
        py::arg("pattern"), py::arg("length"));
    // GIL: trivial - builds a closure
    c.def(
        "SetPatternColor",
        [](std::uint32_t pattern, const std::string& color) {
            return made("SetPatternColor", [=](const project::Project&) {
                return make<project::SetPatternColor>(core::PatternId{pattern},
                                                      parseColorText(color, project::Color{}));
            });
        },
        py::arg("pattern"), py::arg("color"));
}

void playlistCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "AddPlaylistTrackNew",
        [](const std::string& name) {
            return made("AddPlaylistTrack", [=](const project::Project&) {
                project::PlaylistTrack prototype;
                prototype.name = name;
                return make<project::AddPlaylistTrack>(std::move(prototype));
            });
        },
        py::arg("name") = "");
    // GIL: trivial - builds a closure
    c.def(
        "RemovePlaylistTrackId",
        [](std::uint32_t track) {
            return made("RemovePlaylistTrack", [=](const project::Project&) {
                return make<project::RemovePlaylistTrack>(core::PlaylistTrackId{track});
            });
        },
        py::arg("track"));
    // GIL: trivial - builds a closure
    c.def(
        "SetPlaylistTrack",
        [](std::uint32_t track, const py::dict& fields) {
            const auto get = [&fields](const char* key) -> std::optional<py::object> {
                if (fields.contains(key)) {
                    return fields[key];
                }
                return std::nullopt;
            };
            const auto name = get("name");
            const auto color = get("color");
            const auto height = get("height");
            const auto muted = get("muted");
            const auto soloed = get("soloed");
            const auto group = get("group");
            const auto collapsed = get("collapsed");
            std::optional<std::string> nameValue;
            std::optional<std::string> colorValue;
            std::optional<std::uint16_t> heightValue;
            std::optional<bool> mutedValue;
            std::optional<bool> soloValue;
            std::optional<std::uint32_t> groupValue;
            std::optional<bool> collapsedValue;
            if (name) {
                nameValue = name->cast<std::string>();
            }
            if (color) {
                colorValue = color->cast<std::string>();
            }
            if (height) {
                heightValue = height->cast<std::uint16_t>();
            }
            if (muted) {
                mutedValue = muted->cast<bool>();
            }
            if (soloed) {
                soloValue = soloed->cast<bool>();
            }
            if (group) {
                groupValue = group->cast<std::uint32_t>();
            }
            if (collapsed) {
                collapsedValue = collapsed->cast<bool>();
            }
            return made("SetPlaylistTrack", [=](const project::Project& p) {
                const project::PlaylistTrack* current =
                    p.playlist.find(core::PlaylistTrackId{track});
                if (current == nullptr) {
                    throw std::invalid_argument("no playlist track " + std::to_string(track));
                }
                project::PlaylistTrack value = *current;
                value.items.clear();
                value.name = nameValue.value_or(value.name);
                if (colorValue) {
                    value.color = parseColorText(*colorValue, value.color);
                }
                value.height = heightValue.value_or(value.height);
                value.muted = mutedValue.value_or(value.muted);
                value.soloed = soloValue.value_or(value.soloed);
                if (groupValue) {
                    value.group = core::PlaylistTrackId{*groupValue};
                }
                value.collapsed = collapsedValue.value_or(value.collapsed);
                return make<project::SetPlaylistTrack>(core::PlaylistTrackId{track},
                                                       std::move(value));
            });
        },
        py::arg("track"), py::arg("fields"),
        "Set any of name, color, height, muted, soloed, group (0: none), collapsed.");
    // GIL: trivial - builds a closure
    c.def(
        "MovePlaylistTrack",
        [](std::uint32_t track, std::size_t index) {
            return made("MovePlaylistTrack", [=](const project::Project&) {
                return make<project::MovePlaylistTrack>(core::PlaylistTrackId{track}, index);
            });
        },
        py::arg("track"), py::arg("index"));
}

/// Placements on the playlist's tracks: pattern, automation and audio items.
void placementCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "AddPatternItem",
        [](std::uint32_t track, std::uint32_t pattern, std::int64_t start, std::int64_t length) {
            return made("AddPlaylistItem", [=](const project::Project& p) {
                if (p.find(core::PatternId{pattern}) == nullptr) {
                    throw std::invalid_argument("no pattern " + std::to_string(pattern));
                }
                project::PlaylistItem item;
                item.start = core::Ticks{std::max<std::int64_t>(0, start)};
                item.length = core::Ticks{std::max<std::int64_t>(0, length)};
                item.content = project::PatternRef{.pattern = core::PatternId{pattern}};
                return make<project::AddPlaylistItem>(core::PlaylistTrackId{track}, item);
            });
        },
        py::arg("track"), py::arg("pattern"), py::arg("start"), py::arg("length") = 0);
    // GIL: trivial - builds a closure
    c.def(
        "AddAutomationItem",
        [](std::uint32_t track, std::uint32_t clip, std::int64_t start, std::int64_t length) {
            return made("AddPlaylistItem", [=](const project::Project& p) {
                if (p.findAutomationClip(core::AutomationClipId{clip}) == nullptr) {
                    throw std::invalid_argument("no automation clip " + std::to_string(clip));
                }
                project::PlaylistItem item;
                item.start = core::Ticks{std::max<std::int64_t>(0, start)};
                item.length = core::Ticks{std::max<std::int64_t>(0, length)};
                item.content = project::AutomationRef{.clip = core::AutomationClipId{clip}};
                return make<project::AddPlaylistItem>(core::PlaylistTrackId{track}, item);
            });
        },
        py::arg("track"), py::arg("clip"), py::arg("start"), py::arg("length") = 0,
        "Place a playlist automation clip; length 0 holds its last value to the end.");
    // GIL: trivial - builds a closure
    c.def(
        "RemovePlaylistItem",
        [](std::uint32_t track, std::uint32_t item) {
            return made("RemovePlaylistItem", [=](const project::Project&) {
                return make<project::RemovePlaylistItem>(core::PlaylistTrackId{track},
                                                         core::ItemId{item});
            });
        },
        py::arg("track"), py::arg("item"));
    // GIL: trivial - builds a closure
    c.def(
        "SetPlaylistItem",
        [](std::uint32_t track, std::uint32_t item, const py::dict& fields) {
            const auto number = [&fields](const char* key) -> std::optional<double> {
                if (fields.contains(key)) {
                    return fields[key].cast<double>();
                }
                return std::nullopt;
            };
            const auto start = number("start");
            const auto length = number("length");
            const auto offset = number("offset");
            const auto stretch = number("stretch");
            const auto pitch = number("pitch");
            const auto muted = number("muted");
            const auto reverse = number("reverse");
            const auto destination = number("track");
            return made("SetPlaylistItem", [=](const project::Project& p) {
                const project::PlaylistTrack* owner = p.playlist.find(core::PlaylistTrackId{track});
                const project::PlaylistItem* found = nullptr;
                if (owner != nullptr) {
                    const auto match = std::ranges::find(owner->items, core::ItemId{item},
                                                         &project::PlaylistItem::id);
                    found = match == owner->items.end() ? nullptr : &*match;
                }
                if (found == nullptr) {
                    throw std::invalid_argument("no item " + std::to_string(item));
                }
                project::PlaylistItem value = *found;
                const auto ticks = [](double v) {
                    return core::Ticks{std::max<std::int64_t>(0, static_cast<std::int64_t>(v))};
                };
                if (start) {
                    value.start = ticks(*start);
                }
                if (length) {
                    value.length = ticks(*length);
                }
                if (offset) {
                    value.sourceOffset = ticks(*offset);
                }
                if (muted) {
                    value.muted = *muted != 0.0;
                }
                if (auto* audio = std::get_if<project::AudioClipRef>(&value.content)) {
                    if (stretch) {
                        audio->stretch = static_cast<float>(std::clamp(*stretch, 0.05, 20.0));
                    }
                    if (pitch) {
                        audio->pitchSemitones = static_cast<float>(*pitch);
                    }
                    if (reverse) {
                        audio->reverse = *reverse != 0.0;
                    }
                }
                const core::PlaylistTrackId to{
                    destination ? static_cast<std::uint32_t>(*destination) : track};
                return make<project::SetPlaylistItem>(core::PlaylistTrackId{track},
                                                      core::ItemId{item}, std::move(value), to);
            });
        },
        py::arg("track"), py::arg("item"), py::arg("fields"),
        "Set any of start, length, offset, muted, stretch, pitch, reverse, and move to "
        "`track`. Consecutive edits of one item coalesce into one undo step.");
}

void mixerCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "AddInsertNew",
        [](const std::string& name) {
            return made("AddInsert", [=](const project::Project&) {
                project::Insert prototype;
                prototype.name = name;
                return make<project::AddInsert>(std::move(prototype));
            });
        },
        py::arg("name"), "An insert, routed nowhere: route it with AddRoute in the same group.");
    // GIL: trivial - builds a closure
    c.def(
        "RemoveInsert",
        [](std::uint32_t insert) {
            return made("RemoveInsert", [=](const project::Project&) {
                return make<project::RemoveInsert>(core::InsertId{insert});
            });
        },
        py::arg("insert"));
    // GIL: trivial - builds a closure
    c.def(
        "RenameInsert",
        [](std::uint32_t insert, const std::string& name) {
            return made("RenameInsert", [=](const project::Project&) {
                return make<project::RenameInsert>(core::InsertId{insert}, name);
            });
        },
        py::arg("insert"), py::arg("name"));
    // GIL: trivial - builds a closure
    c.def(
        "SetInsertField",
        [](std::uint32_t insert, const std::string& field, double value) {
            static const std::array<std::pair<const char*, project::InsertField>, 6> kFields{{
                {"gain", project::InsertField::Gain},
                {"pan", project::InsertField::Pan},
                {"width", project::InsertField::StereoSeparation},
                {"mute", project::InsertField::Muted},
                {"solo", project::InsertField::Soloed},
                {"invert", project::InsertField::PolarityInvert},
            }};
            const auto match = std::ranges::find(
                kFields, field, [](const auto& entry) { return std::string_view(entry.first); });
            if (match == kFields.end()) {
                throw std::invalid_argument("unknown insert field " + field);
            }
            const project::InsertField which = match->second;
            return made("SetInsertValue", [=](const project::Project&) {
                return make<project::SetInsertValue>(core::InsertId{insert}, which, value);
            });
        },
        py::arg("insert"), py::arg("field"), py::arg("value"),
        "field: gain, pan, width, mute, solo or invert.");
}

/// What runs on an insert and where it goes: effect slots, sends and routes.
void routingCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "AddSlot",
        [](std::uint32_t insert, const std::string& type) {
            return made("AddSlot", [=](const project::Project&) {
                project::Slot prototype;
                prototype.type = type;
                return make<project::AddSlot>(core::InsertId{insert}, std::move(prototype));
            });
        },
        py::arg("insert"), py::arg("type"));
    // GIL: trivial - builds a closure
    c.def(
        "RemoveSlot",
        [](std::uint32_t slot) {
            return made("RemoveSlot", [=](const project::Project&) {
                return make<project::RemoveSlot>(core::SlotId{slot});
            });
        },
        py::arg("slot"));
    // GIL: trivial - builds a closure
    c.def(
        "SetSlotMix",
        [](std::uint32_t slot, double mix) {
            return made("SetSlotValue", [=](const project::Project&) {
                return make<project::SetSlotValue>(core::SlotId{slot}, project::SlotField::Mix,
                                                   mix);
            });
        },
        py::arg("slot"), py::arg("mix"));
    // GIL: trivial - builds a closure
    c.def(
        "SetSlotBypass",
        [](std::uint32_t slot, bool bypass) {
            return made("SetSlotValue", [=](const project::Project&) {
                return make<project::SetSlotValue>(core::SlotId{slot}, project::SlotField::Bypass,
                                                   bypass ? 1.0 : 0.0);
            });
        },
        py::arg("slot"), py::arg("bypass"));
    // GIL: trivial - builds a closure
    c.def(
        "SetSlotParam",
        [](std::uint32_t slot, const std::string& name, double value) {
            return made("SetSlotValue", [=](const project::Project&) {
                return make<project::SetSlotValue>(core::SlotId{slot}, name, value);
            });
        },
        py::arg("slot"), py::arg("name"), py::arg("value"));
    // GIL: trivial - builds a closure
    c.def(
        "SetSlotSidechain",
        [](std::uint32_t slot, std::uint32_t key) {
            return made("SetSlotSidechain", [=](const project::Project&) {
                return make<project::SetSlotSidechain>(core::SlotId{slot}, core::InsertId{key});
            });
        },
        py::arg("slot"), py::arg("key"));
    // GIL: trivial - builds a closure
    c.def(
        "MoveSlot",
        [](std::uint32_t slot, std::size_t index) {
            return made("MoveSlot", [=](const project::Project&) {
                return make<project::MoveSlot>(core::SlotId{slot}, index);
            });
        },
        py::arg("slot"), py::arg("index"));
    // GIL: trivial - builds a closure
    c.def(
        "AddSend",
        [](std::uint32_t insert, std::uint32_t target, float level, bool pre) {
            return made("AddSend", [=](const project::Project&) {
                return make<project::AddSend>(core::InsertId{insert}, core::InsertId{target}, level,
                                              pre);
            });
        },
        py::arg("insert"), py::arg("target"), py::arg("level") = 0.5F, py::arg("pre") = false);
    // GIL: trivial - builds a closure
    c.def(
        "RemoveSend",
        [](std::uint32_t send) {
            return made("RemoveSend", [=](const project::Project&) {
                return make<project::RemoveSend>(core::SendId{send});
            });
        },
        py::arg("send"));
    // GIL: trivial - builds a closure
    c.def(
        "SetSendLevel",
        [](std::uint32_t send, float level) {
            return made("SetSendLevel", [=](const project::Project&) {
                return make<project::SetSendLevel>(core::SendId{send}, level);
            });
        },
        py::arg("send"), py::arg("level"));
    // GIL: trivial - builds a closure
    c.def(
        "AddRoute",
        [](std::uint32_t from, std::uint32_t to) {
            return made("AddRoute", [=](const project::Project&) {
                return make<project::AddRoute>(core::InsertId{from}, core::InsertId{to});
            });
        },
        py::arg("source"), py::arg("target"));
    // GIL: trivial - builds a closure
    c.def(
        "RemoveRoute",
        [](std::uint32_t route) {
            return made("RemoveRoute", [=](const project::Project&) {
                return make<project::RemoveRoute>(core::RouteId{route});
            });
        },
        py::arg("route"));
}

void automationCommands(py::module_& c) {
    // GIL: trivial - builds a closure; the points convert once
    c.def(
        "AddAutomationClip",
        [](std::uint32_t pattern, const std::string& target, const PointsArray& points) {
            const std::vector<project::Breakpoint> breakpoints = breakpointsOf(points);
            return made("AddAutomationClip", [=](const project::Project& p) {
                project::AutomationClip clip;
                clip.targetPath = target;
                clip.target = resolveOrThrow(p, target);
                clip.points = breakpoints;
                return make<project::AddAutomationClip>(core::PatternId{pattern}, std::move(clip));
            });
        },
        py::arg("pattern"), py::arg("target"), py::arg("points"),
        "A lane in a pattern, or in the playlist when pattern is 0.");
    // GIL: trivial - builds a closure
    c.def(
        "RemoveAutomationClip",
        [](std::uint32_t clip) {
            return made("RemoveAutomationClip", [=](const project::Project&) {
                return make<project::RemoveAutomationClip>(core::AutomationClipId{clip});
            });
        },
        py::arg("clip"));
    // GIL: trivial - builds a closure; the points convert once
    c.def(
        "SetBreakpoints",
        [](std::uint32_t clip, const PointsArray& points) {
            const std::vector<project::Breakpoint> breakpoints = breakpointsOf(points);
            return made("SetBreakpoints", [=](const project::Project&) {
                return make<project::SetBreakpoints>(core::AutomationClipId{clip}, breakpoints);
            });
        },
        py::arg("clip"), py::arg("points"),
        "Replace a lane's points with an (n, 2..8) array. Consecutive edits coalesce.");
    // GIL: trivial - builds a closure; each envelope's points convert once
    c.def(
        "SetClipEnvelopes",
        [](std::uint32_t track, std::uint32_t item, const py::list& envelopes) {
            std::vector<std::pair<std::string, std::vector<project::Breakpoint>>> parsed;
            for (const py::handle entry : envelopes) {
                const auto pair = entry.cast<py::tuple>();
                parsed.emplace_back(pair[0].cast<std::string>(),
                                    breakpointsOf(pair[1].cast<PointsArray>()));
            }
            return made("SetClipEnvelopes", [=](const project::Project& p) {
                std::vector<project::ClipEnvelope> out;
                for (const auto& [target, points] : parsed) {
                    project::ClipEnvelope envelope;
                    if (!project::clipTargetFromString(target, envelope.local)) {
                        envelope.local = project::ClipTarget::Param;
                        envelope.targetPath = target;
                        envelope.target = resolveOrThrow(p, target);
                    }
                    envelope.points = points;
                    out.push_back(std::move(envelope));
                }
                return make<project::SetClipEnvelopes>(core::PlaylistTrackId{track},
                                                       core::ItemId{item}, std::move(out));
            });
        },
        py::arg("track"), py::arg("item"), py::arg("envelopes"),
        "Replace a placement's envelopes: a list of (target, points) - target is gain, pan, "
        "pitch or a parameter path.");
}

void projectCommands(py::module_& c) {
    // GIL: trivial - builds a closure
    c.def(
        "RemoveMarker",
        [](std::uint32_t marker) {
            return made("RemoveMarker", [=](const project::Project&) {
                return make<project::RemoveMarker>(core::MarkerId{marker});
            });
        },
        py::arg("marker"));
    // GIL: trivial - builds a closure
    c.def(
        "MoveMarker",
        [](std::uint32_t marker, std::int64_t at, const std::string& name) {
            return made("MoveMarker", [=](const project::Project&) {
                return make<project::MoveMarker>(core::MarkerId{marker},
                                                 core::Ticks{std::max<std::int64_t>(0, at)}, name);
            });
        },
        py::arg("marker"), py::arg("at"), py::arg("name"));
    // GIL: trivial - builds a closure
    c.def(
        "SetMeter",
        [](std::int64_t at, std::uint16_t numerator, std::uint16_t denominator) {
            return made("SetMeterEvent", [=](const project::Project&) {
                return make<project::SetMeterEvent>(core::Ticks{at}, numerator, denominator);
            });
        },
        py::arg("at"), py::arg("numerator"), py::arg("denominator"));
    // GIL: trivial - builds a closure
    c.def(
        "SetProjectInfo",
        [](const py::dict& fields) {
            std::optional<std::string> title;
            std::optional<std::string> author;
            std::optional<double> tuning;
            std::optional<std::tuple<bool, std::int64_t, std::int64_t>> loop;
            if (fields.contains("title")) {
                title = fields["title"].cast<std::string>();
            }
            if (fields.contains("author")) {
                author = fields["author"].cast<std::string>();
            }
            if (fields.contains("tuning")) {
                tuning = fields["tuning"].cast<double>();
            }
            if (fields.contains("loop")) {
                loop = fields["loop"].cast<std::tuple<bool, std::int64_t, std::int64_t>>();
            }
            return made("SetMeta", [=](const project::Project& p) {
                project::ProjectMeta meta = p.meta;
                meta.title = title.value_or(meta.title);
                meta.author = author.value_or(meta.author);
                meta.tuning = std::clamp(tuning.value_or(meta.tuning), 300.0, 600.0);
                if (loop) {
                    meta.loopEnabled = std::get<0>(*loop);
                    meta.loopStart = core::Ticks{std::get<1>(*loop)};
                    meta.loopEnd = core::Ticks{std::get<2>(*loop)};
                }
                return make<project::SetMeta>(std::move(meta));
            });
        },
        py::arg("fields"), "Set any of title, author, tuning, loop=(enabled, start, end).");
    // GIL: trivial - builds a closure
    c.def(
        "AddArrangement",
        [](const std::string& name, bool copy) {
            return made("AddArrangement", [=](const project::Project&) {
                return make<project::AddArrangement>(name, copy);
            });
        },
        py::arg("name"), py::arg("copy") = true);
    // GIL: trivial - builds a closure
    c.def(
        "RemoveArrangement",
        [](std::size_t index) {
            return made("RemoveArrangement", [=](const project::Project&) {
                return make<project::RemoveArrangement>(index);
            });
        },
        py::arg("index"));
    // GIL: trivial - builds a closure
    c.def(
        "SwitchArrangement",
        [](std::size_t index) {
            return made("SwitchArrangement", [=](const project::Project&) {
                return make<project::SwitchArrangement>(index);
            });
        },
        py::arg("index"));
    // GIL: trivial - builds a closure
    c.def(
        "RenameArrangement",
        [](std::optional<std::size_t> index, const std::string& name) {
            return made("RenameArrangement", [=](const project::Project&) {
                return make<project::RenameArrangement>(index, name);
            });
        },
        py::arg("index"), py::arg("name"), "index None renames the live arrangement.");
    // GIL: trivial - builds a closure
    c.def(
        "AddController",
        [](const py::dict& fields) {
            // Read at execute(), which builds with the GIL held (project.cpp).
            auto copy = std::make_shared<py::dict>(fields);
            return made("AddController", [copy](const project::Project& p) {
                return make<project::AddController>(
                    controllerFrom(p, *copy, project::Controller{}));
            });
        },
        py::arg("fields"),
        "A controller from a dict of name, kind, target, depth, center, shape, period, phase, "
        "source_channel, source_insert, attack, decay, sustain, release, gain.");
    // GIL: trivial - builds a closure
    c.def(
        "RemoveController",
        [](std::uint32_t controller) {
            return made("RemoveController", [=](const project::Project&) {
                return make<project::RemoveController>(core::ControllerId{controller});
            });
        },
        py::arg("controller"));
    // GIL: trivial - builds a closure
    c.def(
        "SetController",
        [](std::uint32_t controller, const py::dict& fields) {
            auto copy = std::make_shared<py::dict>(fields);
            return made("SetController", [copy, controller](const project::Project& p) {
                const project::Controller* current = p.find(core::ControllerId{controller});
                if (current == nullptr) {
                    throw std::invalid_argument("no controller " + std::to_string(controller));
                }
                return make<project::SetController>(core::ControllerId{controller},
                                                    controllerFrom(p, *copy, *current));
            });
        },
        py::arg("controller"), py::arg("fields"),
        "Change any controller fields (see AddController).");
}

// NOLINTEND(bugprone-exception-escape)

} // namespace

namespace adx::bindings {

void registerEditCommands(py::module_& m) {
    auto commands = m.attr("commands").cast<py::module_>();
    channelCommands(commands);
    instrumentCommands(commands);
    patternCommands(commands);
    playlistCommands(commands);
    placementCommands(commands);
    mixerCommands(commands);
    routingCommands(commands);
    automationCommands(commands);
    projectCommands(commands);
}

} // namespace adx::bindings
