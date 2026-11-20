// Direct test of user-preset save / load / delete (compiled together with the
// plugin's processor and editor sources; uses a temporary preset folder).

#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
    if (! ok) ++failures;
}

static float get (CleanToneAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
static void set (CleanToneAudioProcessor& p, const char* id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

int main (int argc, char* argv[])
{
    ScopedJuceInitialiser_GUI init;
    auto folder = File::getSpecialLocation (File::tempDirectory).getChildFile ("CleanTonePresetTest");
    folder.deleteRecursively();
    CleanToneAudioProcessor::setUserPresetFolderOverride (folder);

    CleanToneAudioProcessor p;
    p.prepareToPlay (48000.0, 512);

    std::cout << "User presets\n";
    check (p.getUserPresetNames().isEmpty(), "starts with no user presets");

    p.setCurrentProgram (2);
    check (p.getCurrentPresetName() == "Warm Jazz Humbucker" && ! p.isCurrentPresetUser(), "factory preset selected");
    check (! p.isPresetModified(), "factory preset not marked edited");
    set (p, IDs::bass, 5.5f);
    set (p, IDs::cab, 3);
    set (p, IDs::dlyOn, 1);
    set (p, IDs::outGain, -6.0f);
    check (p.isPresetModified(), "moving a knob marks it edited");

    check (p.saveUserPreset ("My Tone"), "save \"My Tone\"");
    check (folder.getChildFile ("My Tone.ctpreset").existsAsFile(), "file written to preset folder");
    check (p.getUserPresetNames() == StringArray { "My Tone" }, "listed in My Presets");
    check (p.isCurrentPresetUser() && p.getCurrentPresetName() == "My Tone", "now showing \"My Tone\"");
    check (! p.isPresetModified(), "not marked edited right after saving");

    p.setCurrentProgram (0);
    check (std::abs (get (p, IDs::bass)) < 1e-4f && get (p, IDs::cab) == 1.0f, "factory preset resets the knobs");
    check (std::abs (get (p, IDs::outGain) + 6.0f) < 0.01f, "Output kept when switching presets");

    set (p, IDs::outGain, 3.0f);
    check (p.loadUserPreset ("My Tone"), "load \"My Tone\"");
    check (std::abs (get (p, IDs::bass) - 5.5f) < 0.01f, "Bass restored (+5.5 dB)");
    check (get (p, IDs::cab) == 3.0f, "Cabinet restored (4x12)");
    check (get (p, IDs::dlyOn) > 0.5f, "Delay switch restored");
    check (std::abs (get (p, IDs::midFreq) - 500.0f) < 1.0f, "untouched knob from the base preset restored (Mid Freq 500 Hz)");
    check (std::abs (get (p, IDs::outGain) - 3.0f) < 0.01f, "Output not changed by loading a user preset");
    check (! p.isPresetModified(), "not marked edited after loading");

    // compressor On switch belongs to the user's setup: presets never flip it
    set (p, IDs::compOn, 0);
    p.setCurrentProgram (5);   // Endless Sustain
    check (get (p, IDs::compOn) < 0.5f, "Compressor stays off when choosing a factory preset");
    p.loadUserPreset ("My Tone");
    check (get (p, IDs::compOn) < 0.5f, "Compressor stays off when loading a user preset");
    check (! p.isPresetModified(), "switching the compressor doesn't mark a preset edited");
    check (! folder.getChildFile ("My Tone.ctpreset").loadFileAsString().contains ("compOn"), "preset files don't store the compressor switch");
    set (p, IDs::compOn, 1);
    check (get (p, IDs::compOn) > 0.5f, "Compressor back on");

    // overwrite
    set (p, IDs::bass, -2.0f);
    check (p.saveUserPreset ("My Tone"), "overwrite \"My Tone\"");
    p.setCurrentProgram (0);
    p.loadUserPreset ("My Tone");
    check (std::abs (get (p, IDs::bass) + 2.0f) < 0.01f, "overwritten value loads (-2 dB)");
    check (p.getUserPresetNames().size() == 1, "still one preset after overwriting");

    // awkward names
    check (p.saveUserPreset ("AC/DC: Clean? <live>"), "save a name with characters Windows forbids");
    const auto names = p.getUserPresetNames();
    check (names.size() == 2, "two presets listed: " + names.joinIntoString (", "));
    String weird; for (auto& n : names) if (n != "My Tone") weird = n;
    check (p.loadUserPreset (weird), "that preset loads back (\"" + weird + "\")");
    check (! p.saveUserPreset ("   "), "blank name refused");

    // a second instance (e.g. the app vs. Cubase) sees the same presets
    {
        CleanToneAudioProcessor other;
        check (other.getUserPresetNames() == p.getUserPresetNames(), "another instance sees the same presets");
    }

    // project save / restore remembers which user preset was active
    p.loadUserPreset ("My Tone");
    MemoryBlock state;
    p.getStateInformation (state);
    {
        CleanToneAudioProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        check (restored.isCurrentPresetUser() && restored.getCurrentPresetName() == "My Tone", "project reload remembers \"My Tone\"");
        check (std::abs (get (restored, IDs::bass) + 2.0f) < 0.01f, "project reload restores the knobs");
    }

    // delete
    check (p.deleteUserPreset ("My Tone"), "delete \"My Tone\"");
    check (! p.userPresetExists ("My Tone") && p.getUserPresetNames().size() == 1, "gone from the list");
    check (std::abs (get (p, IDs::bass) + 2.0f) < 0.01f, "deleting doesn't change the current sound");
    check (! p.loadUserPreset ("My Tone"), "loading a deleted preset fails safely");
    check (! p.loadUserPreset ("does not exist"), "loading a missing preset fails safely");

    // a corrupt file must not crash or change anything
    folder.getChildFile ("Broken.ctpreset").replaceWithText ("not xml at all <<<");
    const float before = get (p, IDs::bass);
    check (! p.loadUserPreset ("Broken") && get (p, IDs::bass) == before, "corrupt preset file ignored");

    // the editor builds and paints with user presets present
    std::cout << "\nEditor\n";
    {
        std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
        check (ed != nullptr, "editor created");
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        check (img.isValid(), "editor paints");
        auto savePng = [] (const Image& im, const char* path)
        {
            File out (path);
            out.deleteFile();
            if (auto os = out.createOutputStream()) PNGImageFormat().writeImageToStream (im, *os);
        };
        if (argc > 1) savePng (img, argv[1]);

        // press Save to open the naming bar, and render that too
        // the controls live inside the editor's scaled body component, so look one level down too
        Array<Component*> all;
        for (auto* c : ed->getChildren()) { all.add (c); for (auto* cc : c->getChildren()) all.add (cc); }
        TextButton* saveBtn = nullptr;
        for (auto* c : all)
            if (auto* b = dynamic_cast<TextButton*> (c))
                if (b->isVisible() && b->getButtonText() == "Save") saveBtn = b;
        check (saveBtn != nullptr, "Save button present");
        if (saveBtn != nullptr && saveBtn->onClick) saveBtn->onClick();
        bool editorShown = false;
        for (auto* c : all)
            if (auto* te = dynamic_cast<TextEditor*> (c)) editorShown = editorShown || te->isVisible();
        check (editorShown, "Save opens the name box");
        if (argc > 2) savePng (ed->createComponentSnapshot ({ 0, 0, ed->getWidth(), 60 }, true, 1.0f), argv[2]);
    }

    folder.deleteRecursively();
    std::cout << "\n" << (failures == 0 ? "ALL PRESET TESTS PASSED" : String (failures) + " FAILURE(S)") << "\n";
    return failures == 0 ? 0 : 1;
}
