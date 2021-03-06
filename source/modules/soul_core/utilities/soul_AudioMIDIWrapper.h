/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul
{

struct MIDIEventInputList
{
    const MIDIEvent* listStart = nullptr;
    const MIDIEvent* listEnd = nullptr;

    const MIDIEvent* begin() const      { return listStart; }
    const MIDIEvent* end() const        { return listEnd; }

    MIDIEventInputList removeEventsBefore (uint32_t frameIndex)
    {
        auto i = listStart;

        while (i != listEnd && i->frameIndex < frameIndex)
            ++i;

        auto oldStart = listStart;
        listStart = i;
        return { oldStart, i };
    }
};

//==============================================================================
struct MIDIEventOutputList
{
    MIDIEvent* start = nullptr;
    uint32_t capacity = 0;

    bool addEvent (MIDIEvent e)
    {
        if (capacity == 0)
            return false;

        *start++ = e;
        --capacity;
        return true;
    }
};

//==============================================================================
struct AudioInputList
{
    AudioInputList() = default;

    void initialise (Performer& p, uint32_t maxBlockSize)
    {
        clear();

        for (auto& e : getInputEndpointsOfType (p, InputEndpointType::audio))
        {
            auto numChannels = getNumAudioChannels (e);
            const auto& frameType = e.getFrameType();
            SOUL_ASSERT (numChannels != 0 && numChannels == frameType.getNumElements());
            SOUL_ASSERT (frameType.isFloat() || (frameType.isVector() && frameType.getElementType().isFloat()));

            inputs.push_back ({ p.getEndpointHandle (e.endpointID), totalNumChannels, numChannels, {} });

            if (numChannels != 1)
                inputs.back().interleaved.resize ({ numChannels, maxBlockSize });

            totalNumChannels += numChannels;
        }
    }

    void clear()
    {
        inputs.clear();
        totalNumChannels = 0;
    }

    void addToFIFO (MultiEndpointFIFO& fifo, uint64_t time, choc::buffer::ChannelArrayView<const float> inputChannels)
    {
        auto numFrames = inputChannels.getNumFrames();

        for (auto& input : inputs)
        {
            if (input.numChannels == 1)
            {
                auto channel = inputChannels.getChannel (input.startChannelIndex);

                fifo.addInputData (input.endpoint, time,
                                   choc::value::createArrayView (const_cast<float*> (channel.data.data), numFrames));
            }
            else
            {
                copy (input.interleaved.getStart (numFrames),
                      inputChannels.getChannelRange ({ input.startChannelIndex, input.startChannelIndex + input.numChannels }));

                fifo.addInputData (input.endpoint, time,
                                   choc::value::create2DArrayView (input.interleaved.getView().data.data,
                                                                   numFrames, input.numChannels));
            }
        }
    }

    struct AudioInput
    {
        EndpointHandle endpoint;
        uint32_t startChannelIndex, numChannels;
        choc::buffer::InterleavedBuffer<float> interleaved;
    };

    std::vector<AudioInput> inputs;
    uint32_t totalNumChannels = 0;
};

//==============================================================================
struct AudioOutputList
{
    AudioOutputList() = default;

    void initialise (Performer& p)
    {
        clear();

        for (auto& e : getOutputEndpointsOfType (p, OutputEndpointType::audio))
        {
            auto numChannels = getNumAudioChannels (e);
            const auto& frameType = e.getFrameType();
            SOUL_ASSERT (numChannels != 0 && numChannels == frameType.getNumElements());
            SOUL_ASSERT (frameType.isFloat() || (frameType.isVector() && frameType.getElementType().isFloat()));

            outputs.push_back ({ p.getEndpointHandle (e.endpointID), totalNumChannels, numChannels });
            totalNumChannels += numChannels;
        }
    }

    void clear()
    {
        outputs.clear();
        totalNumChannels = 0;
    }

    void handleOutputData (Performer& p, choc::buffer::ChannelArrayView<float> outputChannels)
    {
        for (auto& output : outputs)
            copyIntersectionAndClearOutside (outputChannels.getChannelRange ({ output.startChannel, output.startChannel + output.numChannels }),
                                             getChannelSetFromArray (p.getOutputStreamFrames (output.endpoint)));
    }

    struct AudioOutput
    {
        EndpointHandle endpoint;
        uint32_t startChannel, numChannels;
    };

    uint32_t totalNumChannels = 0;
    std::vector<AudioOutput> outputs;
};

//==============================================================================
struct MIDIInputList
{
    MIDIInputList() = default;

    void initialise (Performer& p)
    {
        clear();

        for (auto& e : getInputEndpointsOfType (p, InputEndpointType::midi))
            inputs.push_back ({ p.getEndpointHandle (e.endpointID),
                                choc::value::Value (e.getSingleEventType()) });
    }

    void clear()
    {
        inputs.clear();
    }

    void addToFIFO (MultiEndpointFIFO& fifo, uint64_t time, MIDIEventInputList midiEvents)
    {
        if (inputs.empty())
            return;

        for (auto e : midiEvents)
        {
            auto eventTime = time + e.frameIndex;

            for (auto& input : inputs)
            {
                input.midiEvent.getObjectMemberAt (0).value.set (e.getPackedMIDIData());
                fifo.addInputData (input.endpoint, eventTime, input.midiEvent);
            }
        }
    }

    struct MIDIInput
    {
        EndpointHandle endpoint;
        choc::value::Value midiEvent;
    };

    std::vector<MIDIInput> inputs;
};

//==============================================================================
struct MIDIOutputList
{
    MIDIOutputList() = default;

    void initialise (Performer& p)
    {
        clear();

        for (auto& e : getOutputEndpointsOfType (p, OutputEndpointType::midi))
            outputs.push_back (p.getEndpointHandle (e.endpointID));
    }

    void clear()
    {
        outputs.clear();
    }

    void handleOutputData (Performer& p, uint32_t startFrame, MIDIEventOutputList& midiOut)
    {
        if (midiOut.capacity == 0)
            return;

        for (auto& output : outputs)
        {
            p.iterateOutputEvents (output, [=, &midiOut] (uint32_t frameOffset, const choc::value::ValueView& event) -> bool
            {
                return midiOut.addEvent (MIDIEvent::fromPackedMIDIData (startFrame + frameOffset,
                                                                        event["midiBytes"].getInt32()));
            });
        }
    }

    std::vector<EndpointHandle> outputs;
};

//==============================================================================
struct EventOutputList
{
    EventOutputList() = default;

    void clear()
    {
        fifo.reset (128 * 1024, 4096);
        endpointNames.clear();
    }

    void initialise (Performer& p)
    {
        for (auto& e : getOutputEndpointsOfType (p, OutputEndpointType::event))
        {
            if (! isMIDIEventEndpoint (e))
            {
                auto handle = p.getEndpointHandle (e.endpointID);
                outputs.push_back (handle);
                endpointNames[handle.getRawHandle()] = e.name;
            }
        }
    }

    bool postOutputEvents (Performer& p, uint64_t position)
    {
        bool success = true;

        for (auto& output : outputs)
        {
            p.iterateOutputEvents (output, [&] (uint32_t frameOffset, const choc::value::ValueView& event) -> bool
            {
                if (! fifo.addInputData (output, position + frameOffset, event))
                    success = false;

                return true;
            });
        }

        return success;
    }

    template <typename HandleEventFn>
    void deliverPendingEvents (HandleEventFn&& handleEvent)
    {
        fifo.iterateAllAvailable ([&] (EndpointHandle endpoint, uint64_t time, const choc::value::ValueView& value)
                                  {
                                      auto name = endpointNames.find (endpoint.getRawHandle());
                                      SOUL_ASSERT (name != endpointNames.end());
                                      handleEvent (time, name->second, value);
                                  });
    }

    std::vector<EndpointHandle> outputs;
    MultiEndpointFIFO fifo;
    std::unordered_map<uint32_t, std::string> endpointNames;
};

//==============================================================================
/** Holds the values for a list of traditional float32 parameters, and efficiently
    allows them to be updated and for changed ones to be iterated.
*/
struct ParameterStateList
{
    ParameterStateList() = default;

    using GetRampLengthForSparseStreamFn = std::function<uint32_t(const EndpointDetails&)>;

    void initialise (soul::Performer& performer, GetRampLengthForSparseStreamFn&& getRampLengthForSparseStreamFn)
    {
        valueHolder = choc::value::createFloat32 (0);

        rampedValueHolder = choc::value::createObject (std::string (rampHolderName),
                                                       "rampFrames", choc::value::createInt32 (0),
                                                       "target", choc::value::createFloat32 (0));

        rampFramesMember = rampedValueHolder.getObjectMemberAt (0).value;
        rampTargetMember = rampedValueHolder.getObjectMemberAt (1).value;

        {
            auto params = getInputEndpointsOfType (performer, InputEndpointType::parameter);

            parameters.clear();
            parameters.reserve (params.size());

            for (auto& parameterInput : params)
            {
                Parameter param;
                param.endpoint = performer.getEndpointHandle (parameterInput.endpointID);

                if (isStream (parameterInput))
                {
                    SOUL_ASSERT (getRampLengthForSparseStreamFn != nullptr);
                    param.rampFrames = getRampLengthForSparseStreamFn (parameterInput);
                }

                parameters.push_back (param);
            }
        }

        std::vector<Parameter*> parameterPtrs;
        parameterPtrs.reserve (parameters.size());

        for (auto& p : parameters)
            parameterPtrs.push_back (std::addressof (p));

        auto dirtyHandles = dirtyList.initialise (parameterPtrs);

        for (size_t i = 0; i < parameters.size(); ++i)
            parameters[i].dirtyListHandle = dirtyHandles[i];
    }

    /** Sets the current value for a parameter, and if the value has changed, marks it as
        needing an update.
    */
    void setParameter (uint32_t parameterIndex, float newValue)
    {
        SOUL_ASSERT (parameterIndex < parameters.size());
        auto& param = parameters[parameterIndex];

        if (param.currentValue != newValue)
        {
            param.currentValue = newValue;
            dirtyList.markAsDirty (param.dirtyListHandle);
        }
    }

    /** Forces the parameter to be marked as needing an update. */
    void markAsChanged (uint32_t parameterIndex)
    {
        SOUL_ASSERT (parameterIndex < parameters.size());
        dirtyList.markAsDirty (parameters[parameterIndex].dirtyListHandle);
    }

    /** Pushes events for any endpoints which have had their value
        modified by setParameter() or markAsChanged().
    */
    void addToFIFO (MultiEndpointFIFO& fifo, uint64_t time)
    {
        while (auto p = dirtyList.popNextDirtyObject())
        {
            if (p->rampFrames == 0)
            {
                valueHolder.getViewReference().set (p->currentValue);
                fifo.addInputData (p->endpoint, time, valueHolder);
            }
            else
            {
                rampFramesMember.set (static_cast<int32_t> (p->rampFrames));
                rampTargetMember.set (p->currentValue);
                fifo.addInputData (p->endpoint, time, rampedValueHolder);
            }
        }
    }

    static constexpr std::string_view rampHolderName { "_RampHolder" };

    static bool setSparseValueIfRampedParameterChange (Performer& p, EndpointHandle endpoint, const choc::value::ValueView& v)
    {
        if (v.getType().isObject() && v.getType().getObjectClassName() == rampHolderName)
        {
            p.setSparseInputStreamTarget (endpoint, v.getObjectMemberAt (1).value,
                                          static_cast<uint32_t> (v.getObjectMemberAt (0).value.getInt32()));
            return true;
        }

        return false;
    }

private:
    struct Parameter
    {
        choc::fifo::DirtyList<Parameter>::Handle dirtyListHandle = {};
        soul::EndpointHandle endpoint;
        float currentValue = 0;
        uint32_t rampFrames = 0;
    };

    std::vector<Parameter> parameters;
    choc::fifo::DirtyList<Parameter> dirtyList;
    choc::value::Value valueHolder, rampedValueHolder;
    choc::value::ValueView rampFramesMember, rampTargetMember;
};


//==============================================================================
struct TimelineEventEndpointList
{
    TimelineEventEndpointList() = default;

    void clear()
    {
        timeSigHandle = {};
        tempoHandle = {};
        transportHandle = {};
        positionHandle = {};

        anyChanges = false;
        sendTimeSig = false;
        sendTempo = false;
        sendTransport = false;
        sendPosition = false;
    }

    void initialise (Performer& p)
    {
        clear();

        for (auto& e : getInputEndpointsOfType (p, InputEndpointType::event))
        {
            if (e.dataTypes.size() == 1)
            {
                auto& type = e.dataTypes.front();

                if (type.isObject())
                {
                    if (TimelineEvents::isTimeSig (type))    timeSigHandle   = p.getEndpointHandle (e.endpointID);
                    if (TimelineEvents::isTempo (type))      tempoHandle     = p.getEndpointHandle (e.endpointID);
                    if (TimelineEvents::isTransport (type))  transportHandle = p.getEndpointHandle (e.endpointID);
                    if (TimelineEvents::isPosition (type))   positionHandle  = p.getEndpointHandle (e.endpointID);
                }
            }
        }
    }

    void applyNewTimeSignature (TimeSignature newTimeSig)
    {
        if (timeSigHandle)
        {
            newTimeSigValue.getObjectMemberAt (0).value.set (static_cast<int32_t> (newTimeSig.numerator));
            newTimeSigValue.getObjectMemberAt (1).value.set (static_cast<int32_t> (newTimeSig.denominator));
            sendTimeSig = true;
            anyChanges = true;
        }
    }

    void applyNewTempo (float newBPM)
    {
        if (tempoHandle)
        {
            newTempoValue.getObjectMemberAt (0).value.set (newBPM);
            sendTempo = true;
            anyChanges = true;
        }
    }

    void applyNewTransportState (TransportState newState)
    {
        if (transportHandle)
        {
            newTransportValue.getObjectMemberAt (0).value.set (static_cast<int32_t> (newState));
            sendTransport = true;
            anyChanges = true;
        }
    }

    void applyNewTimelinePosition (TimelinePosition newPosition)
    {
        if (positionHandle)
        {
            newPositionValue.getObjectMemberAt (0).value.set (newPosition.currentFrame);
            newPositionValue.getObjectMemberAt (1).value.set (newPosition.currentQuarterNote);
            newPositionValue.getObjectMemberAt (2).value.set (newPosition.lastBarStartQuarterNote);
            sendPosition = true;
            anyChanges = true;
        }
    }

    void addToFIFO (MultiEndpointFIFO& fifo, uint64_t time)
    {
        if (anyChanges)
        {
            anyChanges = false;

            if (sendTimeSig)
            {
                sendTimeSig = false;
                fifo.addInputData (timeSigHandle, time, newTimeSigValue);
            }

            if (sendTempo)
            {
                sendTempo = false;
                fifo.addInputData (tempoHandle, time, newTempoValue);
            }

            if (sendTransport)
            {
                sendTransport = false;
                fifo.addInputData (transportHandle, time, newTransportValue);
            }

            if (sendPosition)
            {
                sendPosition = false;
                fifo.addInputData (positionHandle, time, newPositionValue);
            }
        }
    }

    EndpointHandle timeSigHandle, tempoHandle, transportHandle, positionHandle;

    bool anyChanges = false;
    bool sendTimeSig = false;
    bool sendTempo = false;
    bool sendTransport = false;
    bool sendPosition = false;

    choc::value::Value newTimeSigValue   { TimelineEvents::createTimeSigValue() };
    choc::value::Value newTempoValue     { TimelineEvents::createTempoValue() };
    choc::value::Value newTransportValue { TimelineEvents::createTransportValue() };
    choc::value::Value newPositionValue  { TimelineEvents::createPositionValue() };
};

//==============================================================================
/**
    A wrapper to simplify the job of rendering a Performer which only needs to deal
    with a synchronous set of audio, MIDI and parameter data (i.e. standard plugin stuff).
*/
struct AudioMIDIWrapper
{
    AudioMIDIWrapper (Performer& p)  : performer (p) {}
    ~AudioMIDIWrapper() = default;

    void reset()
    {
        totalFramesRendered = 0;
        audioInputList.clear();
        audioOutputList.clear();
        midiInputList.clear();
        midiOutputList.clear();
        timelineEventEndpointList.clear();
        eventOutputList.clear();
        inputFIFO.reset (256 * maxInternalBlockSize, maxInternalBlockSize * 2);
        maxBlockSize = 0;
    }

    std::vector<EndpointDetails> getAudioInputEndpoints()       { return getInputEndpointsOfType (performer, InputEndpointType::audio); }
    std::vector<EndpointDetails> getParameterEndpoints()        { return getInputEndpointsOfType (performer, InputEndpointType::parameter); }
    std::vector<EndpointDetails> getEventInputEndpoints()       { return getInputEndpointsOfType (performer, InputEndpointType::event); }

    std::vector<EndpointDetails> getAudioOutputEndpoints()      { return getOutputEndpointsOfType (performer, OutputEndpointType::audio); }
    std::vector<EndpointDetails> getEventOutputEndpoints()      { return getOutputEndpointsOfType (performer, OutputEndpointType::event); }

    void prepare (uint32_t processorMaxBlockSize,
                  ParameterStateList::GetRampLengthForSparseStreamFn&& getRampLengthForSparseStreamFn)
    {
        reset();
        auto& perf = performer;
        SOUL_ASSERT (processorMaxBlockSize > 0);
        maxBlockSize = std::min (maxInternalBlockSize, processorMaxBlockSize);

        audioInputList.initialise (perf, maxInternalBlockSize);
        audioOutputList.initialise (perf);
        midiInputList.initialise (perf);
        midiOutputList.initialise (perf);
        parameterList.initialise (perf, std::move (getRampLengthForSparseStreamFn));
        timelineEventEndpointList.initialise (perf);
        eventOutputList.initialise (perf);
    }

    void render (choc::buffer::ChannelArrayView<const float> input,
                 choc::buffer::ChannelArrayView<float> output,
                 MIDIEventInputList midiIn,
                 MIDIEventOutputList& midiOut)
    {
        auto numFrames = output.getNumFrames();

        if (numFrames > maxInternalBlockSize)
            return renderInChunks (input, output, midiIn, midiOut);

        SOUL_ASSERT (input.getNumFrames() == numFrames && maxBlockSize != 0);

        audioInputList.addToFIFO (inputFIFO, totalFramesRendered, input);
        midiInputList.addToFIFO (inputFIFO, totalFramesRendered, midiIn);
        parameterList.addToFIFO (inputFIFO, totalFramesRendered);
        timelineEventEndpointList.addToFIFO (inputFIFO, totalFramesRendered);
        uint32_t framesDone = 0;

        inputFIFO.iterateChunks (totalFramesRendered,
                                 numFrames, maxBlockSize,
                                 [&] (uint32_t numFramesToDo)
                                 {
                                     performer.prepare (numFramesToDo);
                                 },
                                 [&] (EndpointHandle endpoint, uint64_t /*itemStart*/, const choc::value::ValueView& value)
                                 {
                                     deliverValueToEndpoint (endpoint, value);
                                 },
                                 [&] (uint32_t numFramesDone)
                                 {
                                     performer.advance();
                                     audioOutputList.handleOutputData (performer, output.getFrameRange ({ framesDone, output.size.numFrames }));
                                     midiOutputList.handleOutputData (performer, framesDone, midiOut);
                                     eventOutputList.postOutputEvents (performer, totalFramesRendered + framesDone);
                                     framesDone += numFramesDone;
                                 });

        totalFramesRendered += framesDone;
    }

    void renderInChunks (choc::buffer::ChannelArrayView<const float> input,
                         choc::buffer::ChannelArrayView<float> output,
                         MIDIEventInputList midiIn,
                         MIDIEventOutputList& midiOut)
    {
        auto numFramesRemaining = output.getNumFrames();

        for (uint32_t start = 0;;)
        {
            auto framesToDo = std::min (numFramesRemaining, maxInternalBlockSize);
            auto endFrame = start + framesToDo;

            render (input.getFrameRange ({ start, endFrame }),
                    output.getFrameRange ({ start, endFrame }),
                    midiIn.removeEventsBefore (endFrame), midiOut);

            if (numFramesRemaining <= framesToDo)
                break;

            start += framesToDo;
            numFramesRemaining -= framesToDo;
        }
    }

    void deliverValueToEndpoint (EndpointHandle endpoint, const choc::value::ValueView& value)
    {
        switch (endpoint.getType())
        {
            case EndpointType::stream:
                if (! ParameterStateList::setSparseValueIfRampedParameterChange (performer, endpoint, value))
                    performer.setNextInputStreamFrames (endpoint, value);

                break;

            case EndpointType::event:
                performer.addInputEvent (endpoint, value);
                break;

            case EndpointType::value:
                performer.setInputValue (endpoint, value);
                break;

            case EndpointType::unknown:
            default:
                SOUL_ASSERT_FALSE;
        }
    }

    uint32_t getExpectedNumInputChannels() const     { return audioInputList.totalNumChannels; }
    uint32_t getExpectedNumOutputChannels() const    { return audioOutputList.totalNumChannels; }

    bool postInputEvent (EndpointHandle endpoint, const choc::value::ValueView& value)
    {
        return inputFIFO.addInputData (endpoint, totalFramesRendered, value);
    }

    template <typename HandleEventFn>
    void deliverOutgoingEvents (HandleEventFn&& handleEvent)
    {
        eventOutputList.deliverPendingEvents (handleEvent);
    }

    MultiEndpointFIFO inputFIFO;

    ParameterStateList parameterList;
    TimelineEventEndpointList timelineEventEndpointList;

private:
    //==============================================================================
    Performer& performer;

    AudioInputList   audioInputList;
    MIDIInputList    midiInputList;
    AudioOutputList  audioOutputList;
    MIDIOutputList   midiOutputList;
    EventOutputList  eventOutputList;

    uint64_t totalFramesRendered = 0;
    uint32_t maxBlockSize = 0;

    static constexpr uint32_t maxInternalBlockSize = 512;
};

}
