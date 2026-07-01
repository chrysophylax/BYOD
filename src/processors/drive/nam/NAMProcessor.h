#pragma once

#if ! JUCE_IOS

#include "../../BaseProcessor.h"

namespace NeuralAudio
{
class NeuralModel;
}

class NAMProcessor : public BaseProcessor
{
public:
    explicit NAMProcessor (UndoManager* um = nullptr);
    ~NAMProcessor() override;

    ProcessorType getProcessorType() const override { return Drive; }
    static ParamLayout createParameterLayout();

    void prepare (double sampleRate, int samplesPerBlock) override;
    void processAudio (AudioBuffer<float>& buffer) override;

    std::unique_ptr<XmlElement> toXML() override;
    void fromXML (XmlElement* xml, const chowdsp::Version& version, bool loadPosition) override;

    bool getCustomComponents (OwnedArray<Component>& customComps, chowdsp::HostContextProvider& hcp) override;

    /**
     * Launch the file chooser to pick a model. If @p parent is non-null, the
     * chooser will be modal to that component.
     */
    void chooseModel (Component* parent);

    /**
     * Load a model from an absolute file path. Runs synchronously.
     * On failure, the processor is left in a "no model" pass-through state.
     */
    void loadModelFromFile (const File& file, Component* parent = nullptr);

    /** Unloads the current model. Safe to call from the message thread. */
    void clearModel();

    const String& getCurrentModelName() const { return currentModelName; }

private:
    // Broadcaster used to notify the UI when the loaded model changes.
    using ModelChangeBroadcaster = chowdsp::Broadcaster<void()>;
    ModelChangeBroadcaster modelChangeBroadcaster;

    chowdsp::FloatParameter* inputGainParam = nullptr;
    chowdsp::FloatParameter* outputGainParam = nullptr;
    chowdsp::FloatParameter* qualityParam = nullptr;

    std::array<std::unique_ptr<NeuralAudio::NeuralModel>, 2> models {};
    SpinLock modelChangingMutex;

    String cachedModelPath; // absolute path, empty if no model loaded
    String currentModelName;

    std::shared_ptr<FileChooser> modelChooser;

    chowdsp::Gain<float> inGain, outGain;
    chowdsp::FirstOrderHPF<float> dcBlocker;

    double processSampleRate = 48000.0;
    int processMaxBlockSize = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NAMProcessor)
};

#endif // ! JUCE_IOS
