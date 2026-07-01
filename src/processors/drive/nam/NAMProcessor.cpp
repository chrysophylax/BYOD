#include "NAMProcessor.h"

#if ! JUCE_IOS

#include <NeuralAudio/NeuralModel.h>

#include "gui/utils/ErrorMessageView.h"
#include "gui/utils/ModulatableSlider.h"
#include "processors/ParameterHelpers.h"

namespace NAMTags
{
const String inputGainTag = "nam_input";
const String outputGainTag = "nam_output";
const String qualityTag = "nam_quality";
const String inputCalTag = "nam_input_cal";
const String calibrateTag = "nam_calibrate";
const String modelPathTag = "nam_model_path";
} // namespace NAMTags

NAMProcessor::NAMProcessor (UndoManager* um)
    : BaseProcessor ("NAM", createParameterLayout(), um)
{
    using namespace ParameterHelpers;
    loadParameterPointer (inputGainParam, vts, NAMTags::inputGainTag);
    loadParameterPointer (outputGainParam, vts, NAMTags::outputGainTag);
    loadParameterPointer (qualityParam, vts, NAMTags::qualityTag);
    loadParameterPointer (inputCalDBuParam, vts, NAMTags::inputCalTag);
    loadParameterPointer (calibrateParam, vts, NAMTags::calibrateTag);

    uiOptions.backgroundColour = Colour (0xFF2E2E38);
    uiOptions.powerColour = Colour (0xFF19E5C6);
    uiOptions.info.description =
        "Neural Amp Modeler processor: loads .nam and RTNeural JSON models "
        "(e.g. AIDA-X) and runs them in real time via the NeuralAudio engine.";
    uiOptions.info.authors = StringArray {
        "Steven Atkinson (NAM)",
        "Mike Oliphant (NeuralAudio)",
    };
    uiOptions.info.infoLink = "https://github.com/mikeoliphant/NeuralAudio";

    // The Calibrate toggle is rendered inside the Model ComboBox popup (so we
    // can grey it out when the loaded model doesn't carry calibration data).
    // Hide it from the automatic knob layout to prevent a duplicate button.
    uiOptions.paramIDsToSkip.add (NAMTags::calibrateTag);
}

NAMProcessor::~NAMProcessor() = default;

ParamLayout NAMProcessor::createParameterLayout()
{
    using namespace ParameterHelpers;
    auto params = createBaseParams();

    createGainDBParameter (params, NAMTags::inputGainTag, "Input", -20.0f, 20.0f, 0.0f);
    createGainDBParameter (params, NAMTags::outputGainTag, "Output", -20.0f, 20.0f, 0.0f);
    createPercentParameter (params, NAMTags::qualityTag, "Quality", 1.0f);

    // Input calibration: the user's audio-interface maximum input level in dBu
    // (i.e. the analog level that would drive it to 0 dBFS). NAM's convention
    // uses this with the model's captured input_level_dbu to gain-match the
    // digital signal so the model "sees" the same voltage the real amp did.
    // Typical values: Focusrite Scarlett ~9 dBu, RME Babyface ~13 dBu,
    // pro interfaces up to 24-26 dBu.
    static const auto dBuValToString = [] (float v)
    { return juce::String (v, 1) + " dBu"; };
    static const auto stringTodBuVal = [] (const juce::String& s)
    { return s.getFloatValue(); };
    emplace_param<chowdsp::FloatParameter> (
        params,
        NAMTags::inputCalTag,
        "Input Cal",
        juce::NormalisableRange<float> { 0.0f, 30.0f },
        12.0f,
        std::function<juce::String (float)> { dBuValToString },
        std::function<float (const juce::String&)> { stringTodBuVal });

    emplace_param<chowdsp::BoolParameter> (params, NAMTags::calibrateTag, "Calibrate", true);

    return { params.begin(), params.end() };
}

void NAMProcessor::clearModel()
{
    {
        SpinLock::ScopedLockType lock { modelChangingMutex };
        for (auto& m : models)
            m.reset();
        cachedModelPath.clear();
        currentModelName.clear();
        modelHasCalibration = false;
        modelInputLevelDBu = 12.0f;
        calOutputAdjustDB = 0.0f;
    }
    modelChangeBroadcaster();
}

static void reportNAMError (const String& title, const String& message, Component* parent)
{
    Logger::writeToLog ("[NAM] " + title + ": " + message);

    // Preferred: BYOD's in-plugin ErrorMessageView (requires a live BYODPluginEditor
    // in the parent chain). If parent is null/detached, fall back to JUCE's async
    // native dialog so the user sees SOMETHING.
    if (parent != nullptr)
    {
        ErrorMessageView::showErrorMessage (title, message, "OK", parent);
    }
    else
    {
        NativeMessageBox::showAsync (MessageBoxOptions()
                                         .withIconType (MessageBoxIconType::WarningIcon)
                                         .withTitle (title)
                                         .withMessage (message)
                                         .withButton ("OK"),
                                     nullptr);
    }
}

void NAMProcessor::loadModelFromFile (const File& file, Component* parent)
{
    Logger::writeToLog ("[NAM] loadModelFromFile: " + file.getFullPathName());

    if (file == File {} || ! file.existsAsFile())
    {
        reportNAMError ("NAM Error", "Selected model file does not exist:\n" + file.getFullPathName(), parent);
        return;
    }

    // Build both channel models first; only swap in if BOTH succeed. Prevents
    // a broken half-loaded state on partial failure.
    std::array<std::unique_ptr<NeuralAudio::NeuralModel>, 2> newModels {};

    // Calibration values probed from the newly-loaded model. Only committed
    // to the processor's cached state on full success.
    bool newHasCalibration = false;
    float newModelInputLevelDBu = 12.0f;
    float newCalOutputAdjustDB = 0.0f;

    try
    {
        const auto path = file.getFullPathName().toStdString();
        for (size_t ch = 0; ch < newModels.size(); ++ch)
        {
            Logger::writeToLog ("[NAM] loading channel " + String ((int) ch) + " at "
                                + String (processSampleRate, 1) + " Hz, block "
                                + String (processMaxBlockSize));

            NeuralAudio::NeuralModelLoader loader;
            loader.SetExternalSampleRate ((int) processSampleRate);
            loader.SetDefaultMaxAudioBufferSize (processMaxBlockSize);
            // Use 0 dBu as baseline so we can back-solve modelInputLevelDBu
            // from GetRecommendedInputDBAdjustment() (== 0 - modelInputLevelDBu).
            loader.SetAudioInputLevelDBu (0.0f);

            auto* raw = loader.CreateFromFile (path);
            if (raw == nullptr)
                throw std::runtime_error ("CreateFromFile returned null (unsupported "
                                          "architecture or malformed file)");

            newModels[ch].reset (raw);
            newModels[ch]->SetMaxAudioBufferSize (processMaxBlockSize);
            newModels[ch]->SetQualityScaleFactor (qualityParam != nullptr
                                                      ? qualityParam->getCurrentValue()
                                                      : 1.0f);

            // Probe calibration once (from channel 0). Both channels load the
            // same file, so the values are identical.
            if (ch == 0)
            {
                // GetMetadata returns "" if the key isn't present. NAM v0.5.5+
                // and calibrated Keras models set "input_level_dbu".
                const auto inputLevelStr = newModels[ch]->GetMetadata ("input_level_dbu");
                newHasCalibration = ! inputLevelStr.empty();

                // modelInputLevelDBu = -GetRecommendedInputDBAdjustment() given
                // audioInputLevelDBu was set to 0 on the loader.
                newModelInputLevelDBu = -newModels[ch]->GetRecommendedInputDBAdjustment();

                // Output adjustment (== -18 - modelLoudnessDB) is independent
                // of the user's input calibration setting.
                newCalOutputAdjustDB = newModels[ch]->GetRecommendedOutputDBAdjustment();

                Logger::writeToLog (String ("[NAM] calibration: hasCal=")
                                    + (newHasCalibration ? "yes" : "no")
                                    + ", modelInDBu=" + String (newModelInputLevelDBu, 2)
                                    + ", recommendedOutDB=" + String (newCalOutputAdjustDB, 2));
            }
        }
    }
    catch (const std::exception& exc)
    {
        // newModels is discarded on scope exit -> no partial state committed.
        reportNAMError ("NAM Error",
                        String { "Unable to load NAM model:\n\n" } + exc.what()
                            + "\n\nFile: " + file.getFullPathName(),
                        parent);
        return;
    }
    catch (...)
    {
        reportNAMError ("NAM Error",
                        "Unknown error while loading NAM model.\n\nFile: " + file.getFullPathName(),
                        parent);
        return;
    }

    Logger::writeToLog ("[NAM] load succeeded, swapping in models: "
                        + file.getFileNameWithoutExtension());

    {
        SpinLock::ScopedLockType lock { modelChangingMutex };
        for (size_t ch = 0; ch < models.size(); ++ch)
            models[ch] = std::move (newModels[ch]);
        cachedModelPath = file.getFullPathName();
        currentModelName = file.getFileNameWithoutExtension();
        modelHasCalibration = newHasCalibration;
        modelInputLevelDBu = newModelInputLevelDBu;
        calOutputAdjustDB = newCalOutputAdjustDB;
    }

    modelChangeBroadcaster();
    Logger::writeToLog ("[NAM] broadcaster fired, UI should now show: " + currentModelName);
}

void NAMProcessor::chooseModel (Component* parent)
{
    modelChooser = std::make_shared<FileChooser> ("Load NAM Model",
                                                  File {},
                                                  "*.nam;*.json",
                                                  true,
                                                  false,
                                                  parent);

    Logger::writeToLog ("[NAM] launching file chooser");

    modelChooser->launchAsync (
        FileBrowserComponent::FileChooserFlags::canSelectFiles,
        [this, safeParent = Component::SafePointer { parent }] (const FileChooser& fc)
        {
            const auto file = fc.getResult();
            Logger::writeToLog ("[NAM] file chooser result: "
                                + (file == File {} ? String { "(cancelled)" } : file.getFullPathName()));
            if (file == File {})
                return;
            loadModelFromFile (file, safeParent.getComponent());
        });
}

void NAMProcessor::prepare (double sampleRate, int samplesPerBlock)
{
    processSampleRate = sampleRate;
    processMaxBlockSize = samplesPerBlock;

    const auto spec = dsp::ProcessSpec { sampleRate, (uint32) samplesPerBlock, 2 };

    inGain.prepare (spec);
    inGain.setRampDurationSeconds (0.05);
    outGain.prepare (spec);
    outGain.setRampDurationSeconds (0.05);
    calInGain.prepare (spec);
    calInGain.setRampDurationSeconds (0.05);
    calOutGain.prepare (spec);
    calOutGain.setRampDurationSeconds (0.05);

    dcBlocker.prepare (spec);
    dcBlocker.calcCoefs (20.0f, (float) sampleRate);

    // Reload the current model at the new sample rate / buffer size. WaveNet
    // models bake the sample rate in at load time, so we must recreate.
    if (cachedModelPath.isNotEmpty())
    {
        const auto pathCopy = cachedModelPath;
        try
        {
            std::array<std::unique_ptr<NeuralAudio::NeuralModel>, 2> newModels {};
            bool newHasCalibration = false;
            float newModelInputLevelDBu = 12.0f;
            float newCalOutputAdjustDB = 0.0f;

            for (size_t ch = 0; ch < newModels.size(); ++ch)
            {
                NeuralAudio::NeuralModelLoader loader;
                loader.SetExternalSampleRate ((int) processSampleRate);
                loader.SetDefaultMaxAudioBufferSize (processMaxBlockSize);
                loader.SetAudioInputLevelDBu (0.0f);
                auto* raw = loader.CreateFromFile (pathCopy.toStdString());
                if (raw == nullptr)
                    throw std::runtime_error ("CreateFromFile returned null");
                newModels[ch].reset (raw);
                newModels[ch]->SetMaxAudioBufferSize (processMaxBlockSize);
                newModels[ch]->SetQualityScaleFactor (qualityParam != nullptr
                                                          ? qualityParam->getCurrentValue()
                                                          : 1.0f);

                if (ch == 0)
                {
                    newHasCalibration = ! newModels[ch]->GetMetadata ("input_level_dbu").empty();
                    newModelInputLevelDBu = -newModels[ch]->GetRecommendedInputDBAdjustment();
                    newCalOutputAdjustDB = newModels[ch]->GetRecommendedOutputDBAdjustment();
                }
            }
            SpinLock::ScopedLockType lock { modelChangingMutex };
            for (size_t ch = 0; ch < models.size(); ++ch)
                models[ch] = std::move (newModels[ch]);
            modelHasCalibration = newHasCalibration;
            modelInputLevelDBu = newModelInputLevelDBu;
            calOutputAdjustDB = newCalOutputAdjustDB;
        }
        catch (...)
        {
            SpinLock::ScopedLockType lock { modelChangingMutex };
            for (auto& m : models)
                m.reset();
            modelHasCalibration = false;
        }
    }
}

void NAMProcessor::processAudio (AudioBuffer<float>& buffer)
{
    const SpinLock::ScopedTryLockType lock { modelChangingMutex };
    if (! lock.isLocked())
        return; // model is being swapped; pass audio through unchanged this block

    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();

    // Calibration only kicks in if the toggle is on AND the model carries
    // input_level_dbu metadata. Otherwise the two cal stages are unity.
    const bool applyCalibration = modelHasCalibration && calibrateParam->get();
    const float calInDB = applyCalibration
                              ? (inputCalDBuParam->getCurrentValue() - modelInputLevelDBu)
                              : 0.0f;
    const float calOutDB = applyCalibration ? calOutputAdjustDB : 0.0f;

    // Signal chain: user-in → cal-in → model → cal-out → user-out → DC block
    inGain.setGainDecibels (inputGainParam->getCurrentValue());
    inGain.process (buffer);

    calInGain.setGainDecibels (calInDB);
    calInGain.process (buffer);

    // Model processing (per channel)
    for (int ch = 0; ch < numChannels && ch < (int) models.size(); ++ch)
    {
        if (models[(size_t) ch] == nullptr)
            continue;

        // Keep the quality parameter in sync. NeuralAudio guarantees this is
        // realtime-safe when IsQualityChangeRealtimeSafe() returns true (the
        // default for supported architectures).
        models[(size_t) ch]->SetQualityScaleFactor (qualityParam->getCurrentValue());

        auto* x = buffer.getWritePointer (ch);
        models[(size_t) ch]->Process (x, x, (size_t) numSamples);
    }

    calOutGain.setGainDecibels (calOutDB);
    calOutGain.process (buffer);

    outGain.setGainDecibels (outputGainParam->getCurrentValue());
    outGain.process (buffer);

    // DC blocker
    dcBlocker.processBlock (buffer);
}

std::unique_ptr<XmlElement> NAMProcessor::toXML()
{
    auto xml = BaseProcessor::toXML();
    xml->setAttribute (NAMTags::modelPathTag, cachedModelPath);
    return xml;
}

void NAMProcessor::fromXML (XmlElement* xml, const chowdsp::Version& version, bool loadPosition)
{
    BaseProcessor::fromXML (xml, version, loadPosition);

    const auto savedPath = xml->getStringAttribute (NAMTags::modelPathTag, {});
    if (savedPath.isNotEmpty())
    {
        const File file { savedPath };
        if (file.existsAsFile())
        {
            loadModelFromFile (file);
        }
        else
        {
            Logger::writeToLog ("NAM: saved model path no longer exists: " + savedPath);
            SpinLock::ScopedLockType lock { modelChangingMutex };
            for (auto& m : models)
                m.reset();
            cachedModelPath = savedPath; // remember what was saved so save-round-trips are stable
            currentModelName = "(missing) " + File (savedPath).getFileNameWithoutExtension();
            modelChangeBroadcaster();
        }
    }
}

bool NAMProcessor::getCustomComponents (OwnedArray<Component>& customComps, chowdsp::HostContextProvider& hcp)
{
    using namespace chowdsp::ParamUtils;

    // NOTE: BYOD's KnobsComponent only lays out custom components that are
    // Sliders or ComboBoxes (TextButtons in the customComponents array get
    // zero bounds). So we use a ComboBox here, mirroring GuitarMLAmp's
    // ModelChoiceBox pattern. The popup is built manually (bypassing
    // ComboBox's automatic menu) so we can include a checkable, optionally-
    // disabled "Calibrate Input" toggle.
    class ModelChoiceBox : public ComboBox
    {
    public:
        ModelChoiceBox (NAMProcessor& proc, ModelChangeBroadcaster& caster)
            : processor (proc)
        {
            setText (getDisplayText(), dontSendNotification);
            refreshTooltip();

            modelChangeCallback = caster.connect ([this]
                                                  {
                                                      Logger::writeToLog ("[NAM] UI: model-change broadcast received, refreshing ComboBox to: "
                                                                          + getDisplayText());
                                                      setText (getDisplayText(), dontSendNotification);
                                                      refreshTooltip();
                                                      repaint();
                                                  });

            Component::setName ("nam_model__box");
        }

        // Override the default ComboBox popup so we can inject a checkable
        // Calibrate item that can be dynamically disabled.
        void showPopup() override
        {
            juce::PopupMenu menu;
            menu.setLookAndFeel (&getLookAndFeel());

            menu.addItem (loadItemId, "Load Model...");

            if (processor.getCurrentModelName().isNotEmpty())
                menu.addItem (clearItemId, "Clear Model");

            menu.addSeparator();

            const bool hasCal = processor.currentModelHasCalibration();
            auto* calParam = processor.getCalibrateParam();

            juce::PopupMenu::Item calItem;
            calItem.itemID = calibrateItemId;
            if (hasCal)
            {
                calItem.text = "Calibrate Input  (model: "
                               + juce::String (processor.getModelInputLevelDBu(), 1) + " dBu)";
                calItem.isEnabled = true;
                calItem.isTicked = (calParam != nullptr && calParam->get());
            }
            else
            {
                calItem.text = "Calibrate Input  (model not calibrated)";
                calItem.isEnabled = false;
                calItem.isTicked = false;
            }
            menu.addItem (calItem);

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                                [this] (int result)
                                {
                                    if (result == loadItemId)
                                    {
                                        processor.chooseModel (getTopLevelComponent());
                                    }
                                    else if (result == clearItemId)
                                    {
                                        processor.clearModel();
                                    }
                                    else if (result == calibrateItemId)
                                    {
                                        if (auto* p = processor.getCalibrateParam())
                                        {
                                            const bool newValue = ! p->get();
                                            p->beginChangeGesture();
                                            p->setValueNotifyingHost (newValue ? 1.0f : 0.0f);
                                            p->endChangeGesture();
                                            refreshTooltip();
                                        }
                                    }
                                });
        }

        void refreshTooltip()
        {
            if (! processor.currentModelHasCalibration())
                setTooltip ("Model not calibrated - Calibrate Input control unavailable");
            else
                setTooltip ("Model calibration: "
                            + juce::String (processor.getModelInputLevelDBu(), 1) + " dBu");
        }

        String getDisplayText() const
        {
            const auto& name = processor.getCurrentModelName();
            return name.isEmpty() ? "(no model)" : name;
        }

        void visibilityChanged() override
        {
            setName ("Model");
        }

    private:
        enum ItemIds
        {
            loadItemId = 1,
            clearItemId = 2,
            calibrateItemId = 3,
        };

        NAMProcessor& processor;
        chowdsp::ScopedCallback modelChangeCallback;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModelChoiceBox)
    };

    customComps.add (std::make_unique<ModelChoiceBox> (*this, modelChangeBroadcaster));

    juce::ignoreUnused (hcp);
    return false;
}

#endif // ! JUCE_IOS
