// SPDX-License-Identifier: MIT

// The Variables zone: the starting values of the variables, the scenes, and the changes while a sequence plays.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

#include "app/app.h"
#include "app/scene_values.h"
#include "app/time_text.h"
#include "format/file.h"

namespace hokuto::app
{
namespace
{

// A variable contains 16 bits.
constexpr int kLowestVariable = std::numeric_limits<int16_t>::min();
constexpr int kHighestVariable = std::numeric_limits<int16_t>::max();

// `value` as a variable's value, clamped to 16 bits.
int16_t VariableValue(int value)
{
    return static_cast<int16_t>(std::clamp(value, kLowestVariable, kHighestVariable));
}

// The starting values of a sound's variables as a scene's values. A scene holds the variables that a game sets only.
std::map<uint8_t, int16_t> SceneValues(const song::StartValues& values)
{
    std::map<uint8_t, int16_t> scene;
    for (const auto& [variable, value] : values)
    {
        if (variable >= 0 && variable < format::kSceneVariables)
        {
            scene[static_cast<uint8_t>(variable)] = value;
        }
    }

    return scene;
}

// The first of Scene 1, Scene 2 and so on that no scene of sound `sound` among `scenes` has.
std::string NewSceneName(const std::vector<format::Scene>& scenes, uint32_t sound)
{
    for (int number = 1;; number++)
    {
        const std::string name = "Scene " + std::to_string(number);
        const auto taken = [sound, &name](const format::Scene& scene)
        {
            return scene.sound == sound && scene.name == name;
        };
        if (std::none_of(scenes.begin(), scenes.end(), taken))
        {
            return name;
        }
    }
}

} // namespace

void App::ApplyVariableChoices()
{
    if (!song_)
    {
        return;
    }

    // A variable has its initial value without a choice: -1, or 0 for a global variable on the Wii. A variable holds 16
    // bits.
    const std::optional<uint32_t> sound = ShownSound();
    bool chosen = false;
    for (VariableView& variable : song_->variables)
    {
        const std::optional<int> value = std::exchange(variable.chosen, std::nullopt);
        if (!value || !sound)
        {
            continue;
        }

        song::StartValues& values = start_values_[*sound];
        if (*value == variable.initial)
        {
            values.erase(variable.number);
        }
        else
        {
            values[variable.number] = VariableValue(*value);
        }
        chosen = true;
    }
    if (chosen && sequence_)
    {
        ReadShown();
    }
}

void App::UpdateScenes()
{
    if (!song_)
    {
        return;
    }

    song_->scenes.clear();
    scene_places_.clear();
    const std::optional<uint32_t> sound = ShownSound();
    const format::File* document = Document();
    if (!sound || document == nullptr)
    {
        return;
    }

    const song::StartValues values = StartValuesOf(*sound);
    const song::Changes changes = ChangesOf(*sound);
    for (std::size_t place = 0; place < document->scenes.size(); place++)
    {
        const format::Scene& scene = document->scenes[place];
        if (scene.sound == *sound)
        {
            song_->scenes.push_back({scene.name, SceneStartValues(scene) == values && SceneChanges(scene) == changes});
            scene_places_.push_back(place);
        }
    }
}

void App::ApplySceneRequest()
{
    const std::optional<SceneRequest> request =
        song_ ? std::exchange(song_->scene_request, std::nullopt) : std::nullopt;
    const std::optional<uint32_t> sound = ShownSound();
    const format::File* document = Document();
    if (!request || !sound || document == nullptr)
    {
        return;
    }

    // Store makes a scene of the values that the variables start with, and of their changes. It takes the first free
    // name.
    format::File file = *document;
    const std::string where = "Sound: " + song_->name + " - ";
    if (request->action == SceneAction::kStore)
    {
        const std::string name = NewSceneName(file.scenes, *sound);
        file.scenes.push_back({*sound, name, SceneValues(StartValuesOf(*sound)), FileChanges(ChangesOf(*sound))});
        Do("Store Scene", where + name, std::move(file));
        return;
    }

    // The other actions take a scene of the sound. A scene that the file no longer holds takes none.
    if (request->scene >= scene_places_.size() || scene_places_[request->scene] >= file.scenes.size())
    {
        return;
    }

    const std::size_t place = scene_places_[request->scene];
    format::Scene& scene = file.scenes[place];
    const std::string name = scene.name;
    switch (request->action)
    {
    case SceneAction::kRecall:
        RecallScene(*sound, scene);
        break;

    case SceneAction::kUpdate:
        scene.values = SceneValues(StartValuesOf(*sound));
        scene.changes = FileChanges(ChangesOf(*sound));
        if (scene != document->scenes[place])
        {
            Do("Update Scene", where + name, std::move(file));
        }
        break;

    case SceneAction::kRename:
        scene.name = request->name;
        Do("Rename Scene", where + "from " + name + " to " + request->name, std::move(file));
        break;

    case SceneAction::kDelete:
        file.scenes.erase(file.scenes.begin() + static_cast<std::ptrdiff_t>(place));
        Do("Delete Scene", where + name, std::move(file));
        break;

    case SceneAction::kStore:
        break;
    }
}

void App::RecallScene(uint32_t sound, const format::Scene& scene)
{
    start_values_[sound] = SceneStartValues(scene);
    changes_[sound] = SceneChanges(scene);
    ShowChanges(sound);
    if (sequence_)
    {
        ReadShown();
        return;
    }

    // The made-up song shows the values and nothing more.
    const song::StartValues& values = start_values_[sound];
    for (VariableView& variable : song_->variables)
    {
        const auto found = values.find(variable.number);
        variable.start = found != values.end() ? found->second : -1;
    }
}

void App::UpdateVariables()
{
    if (!song_ || !sequence_)
    {
        return;
    }

    // Each variable keeps the last value that the clock gave until the clock gives another. A stop clears them.
    const std::optional<song::PlaybackClock::Variables> values =
        clock_ ? clock_->VariablesAt(HeardSamples()) : std::nullopt;
    for (VariableView& variable : song_->variables)
    {
        if (!clock_)
        {
            variable.value.reset();
        }
        else if (values && variable.number >= 0 && variable.number < song::kGameVariables)
        {
            variable.value = (*values)[static_cast<std::size_t>(variable.number)];
        }
    }
}

song::StartValues App::StartValuesOf(uint32_t sound) const
{
    const auto found = start_values_.find(sound);
    return found != start_values_.end() ? found->second : song::StartValues();
}

song::Changes App::ChangesOf(uint32_t sound) const
{
    const auto found = changes_.find(sound);
    return found != changes_.end() ? found->second : song::Changes();
}

void App::ApplyChangeRequest()
{
    const std::optional<ChangeRequest> request =
        song_ ? std::exchange(song_->change_request, std::nullopt) : std::nullopt;
    const std::optional<uint32_t> sound = ShownSound();
    if (!request || !sound)
    {
        return;
    }
    if (!request->refused.empty())
    {
        ShowMessage(request->refused);
        return;
    }

    // A variable holds 16 bits. A change's time counts milliseconds in 32 bits.
    song::Changes& changes = changes_[*sound];
    switch (request->action)
    {
    case ChangeAction::kAdd:
        {
            // A new change comes at the position. It sets the first variable that the zone lists, or else the first
            // that the song reads, to the choice after the value that the variable starts with, or to the next value.
            const std::vector<VariableView>& offered =
                song_->variables.empty() ? song_->read_variables : song_->variables;
            if (offered.empty())
            {
                break;
            }

            const VariableView& variable = offered.front();
            const song::StartValues starts = StartValuesOf(*sound);
            const auto found = starts.find(variable.number);
            const int start = found != starts.end() ? found->second : variable.initial;
            const auto chosen = std::find(variable.choices.begin(), variable.choices.end(), start);
            int value = start < kHighestVariable ? start + 1 : start - 1;
            if (!variable.choices.empty())
            {
                value = chosen == variable.choices.end() || std::next(chosen) == variable.choices.end()
                            ? variable.choices.front()
                            : *std::next(chosen);
            }

            // The change takes the time that the display shows.
            const uint64_t time = std::min<uint64_t>(WholeMs(PositionTime()), std::numeric_limits<uint32_t>::max());
            changes.push_back({static_cast<uint32_t>(time), variable.number, VariableValue(value)});
            break;
        }

    case ChangeAction::kEdit:
        if (request->change < changes.size())
        {
            const ChangeView& edited = request->edited;
            changes[request->change] = {edited.time, edited.variable, VariableValue(edited.value)};
        }
        break;

    case ChangeAction::kRemove:
        if (request->change < changes.size())
        {
            changes.erase(changes.begin() + static_cast<std::ptrdiff_t>(request->change));
        }
        break;
    }

    // The changes keep the order of their times. The sequence plays them once it's read again.
    std::ranges::stable_sort(changes, {}, &song::VariableChange::time);
    ShowChanges(*sound);
    if (sequence_)
    {
        ReadShown();
    }
}

void App::ShowChanges(uint32_t sound)
{
    if (!song_)
    {
        return;
    }

    // Without a sequence, the made-up song keeps one tempo.
    song_->changes.clear();
    for (const song::VariableChange& change : ChangesOf(sound))
    {
        const double tick = sequence_ ? song::TickOfMs(*sequence_, change.time)
                                      : change.time * song_->tempo * song_->ticks_per_quarter / 60'000.0;
        song_->changes.push_back({change.time, change.variable, change.value, tick});
    }
}

double App::PositionTime() const
{
    if (sequence_)
    {
        return song::MsOfTick(*sequence_, transport_.Position() * sequence_->timebase);
    }

    return transport_.Position() * 60'000.0 / transport_.Tempo();
}

} // namespace hokuto::app
