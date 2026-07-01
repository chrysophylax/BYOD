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
const String modelPathTag = "nam_model_path";
} // namespace NAMTags

NAMProcessor::NAMProcessor (UndoManager* um)
    : BaseProcessor ("NAM", createParameterLayout(), um)
{
    using namespace ParameterHelpers;
    loadParameterPointer (inputGainParam, vts, NAMTags::inputGainTag);
    loadParameterPointer (outputGainParam, vts, NAMTags::outputGainTag);
    loadParameterPointer (qualityParam, vts, NAMTags::qualityTag);

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
}

NAMProcessor::~NAMProcessor() = default;

ParamLayout NAMProcessor::createParameterLayout()
{
    using namespace ParameterHelpers;
    auto params = createBaseParams();

    createGainDBParameter (params, NAMTags::inputGainTag, "Input", -20.0f, 20.0f, 0.0f);
    createGainDBParameter (params, NAMTags::outputGainTag, "Output", -20.0f, 20.0f, 0.0f);
    createPercentParameter (params, NAMTags::qualityTag, "Quality", 1.0f);

    return { params.begin(), params.end() };
}

void NAMProcessor::clearModel()
{
    SpinLock::ScopedLockType lock { modelChangingMutex };
    for (auto& m : models)
        m.reset();
    cachedModelPath.clear();
    currentModelName.clear();
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

            auto* raw = loader.CreateFromFile (path);
            if (raw == nullptr)
                throw std::runtime_error ("CreateFromFile returned null (unsupported "
                                          "architecture or malformed file)");

            newModels[ch].reset (raw);
            newModels[ch]->SetMaxAudioBufferSize (processMaxBlockSize);
            newModels[ch]->SetQualityScaleFactor (qualityParam != nullptr
                                                      ? qualityParam->getCurrentValue()
                                                      : 1.0f);
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
            for (auto& newModel : newModels)
            {
                NeuralAudio::NeuralModelLoader loader;
                loader.SetExternalSampleRate ((int) processSampleRate);
                loader.SetDefaultMaxAudioBufferSize (processMaxBlockSize);
                auto* raw = loader.CreateFromFile (pathCopy.toStdString());
                if (raw == nullptr)
                    throw std::runtime_error ("CreateFromFile returned null");
                newModel.reset (raw);
                newModel->SetMaxAudioBufferSize (processMaxBlockSize);
                newModel->SetQualityScaleFactor (qualityParam != nullptr
                                                     ? qualityParam->getCurrentValue()
                                                     : 1.0f);
            }
            SpinLock::ScopedLockType lock { modelChangingMutex };
            for (size_t ch = 0; ch < models.size(); ++ch)
                models[ch] = std::move (newModels[ch]);
        }
        catch (...)
        {
            SpinLock::ScopedLockType lock { modelChangingMutex };
            for (auto& m : models)
                m.reset();
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

    // Input gain
    inGain.setGainDecibels (inputGainParam->getCurrentValue());
    inGain.process (buffer);

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

    // Output gain
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
    // ModelChoiceBox pattern.
    class ModelChoiceBox : public ComboBox
    {
    public:
        ModelChoiceBox (NAMProcessor& proc, ModelChangeBroadcaster& caster)
            : processor (proc)
        {
            refreshItems();
            setText (getDisplayText(), dontSendNotification);

            modelChangeCallback = caster.connect ([this]
                                                  {
                                                      Logger::writeToLog ("[NAM] UI: model-change broadcast received, refreshing ComboBox to: "
                                                                          + getDisplayText());
                                                      refreshItems();
                                                      setText (getDisplayText(), dontSendNotification);
                                                      repaint();
                                                  });

            onChange = [this]
            {
                const auto id = getSelectedId();
                // Allow the same item to be picked again next time.
                setSelectedId (0, dontSendNotification);
                setText (getDisplayText(), dontSendNotification);

                if (id == loadItemId)
                    processor.chooseModel (getTopLevelComponent());
                else if (id == clearItemId)
                    processor.clearModel();
            };

            Component::setName ("nam_model__box");
        }

        void refreshItems()
        {
            clear (dontSendNotification);
            addItem ("Load Model...", loadItemId);
            if (processor.getCurrentModelName().isNotEmpty())
                addItem ("Clear Model", clearItemId);
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
