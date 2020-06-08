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

#if ! SOUL_INSIDE_CORE_CPP
 #error "Don't add this cpp file to your build, it gets included indirectly by soul_core.cpp"
#endif

#include <iostream>

namespace soul
{

namespace WaveGenerators
{
    //==============================================================================
    struct Generator
    {
        Generator() = default;
        virtual ~Generator() = default;

        void init (double frequency, double sampleRate)
        {
            phaseIncrement = frequency / sampleRate;
        }

        virtual double getSample() = 0;

        void advance()
        {
            currentPhase += phaseIncrement;

            while (currentPhase >= 1.0)
                currentPhase -= 1.0;
        }

        double phaseIncrement = 0;
        double currentPhase   = 0;
    };


    //==============================================================================
    struct Sine  : public Generator
    {
        double getSample() override
        {
            return std::sin (currentPhase * twoPi);
        }
    };

    //==============================================================================
    struct Blep  : public Generator
    {
        double blep (double phase)
        {
            if (phase < phaseIncrement)
            {
                phase = phase / phaseIncrement;
                return (phase + phase) - (phase * phase) - 1.0f;
            }

            if (phase > (1.0f - phaseIncrement))
            {
                phase = (phase - 1.0f) / phaseIncrement;
                return (phase * phase) + (phase + phase) + 1.0f;
            }

            return 0;
        }
    };

    //==============================================================================
    struct Saw  : public Blep
    {
        double getSample() override
        {
            return -1.0 + (2.0 * currentPhase) - blep (currentPhase);
        }
    };

    //==============================================================================
    struct Square  : public Blep
    {
        double getSample() override
        {
            return (currentPhase < 0.5 ? -1.0f : 1.0f) - blep (currentPhase)
                    + blep (std::fmod (currentPhase + 0.5, 1.0));
        }
    };

    //==============================================================================
    struct Triangle  : public Square
    {
        double getSample() override
        {
            sum += 4.0 * phaseIncrement * Square::getSample();
            return sum;
        }

        double sum = 1;
    };
}

//==============================================================================
template <typename ChannelSet>
choc::value::Value createArrayFromChannelSet (ChannelSet source, uint32_t targetNumChans)
{
    if (targetNumChans <= source.numChannels)
        return choc::value::Value::createArray (source.numFrames, targetNumChans,
                                                [&] (uint32_t frame, uint32_t chan) { return source.getSample (chan, frame); });

    if (source.numChannels == 1 && targetNumChans == 2)
        return choc::value::Value::createArray (source.numFrames, targetNumChans,
                                                [&] (uint32_t frame, uint32_t) { return source.getSample (0, frame); });

    return choc::value::Value::createArray (source.numFrames, targetNumChans,
                                            [&] (uint32_t frame, uint32_t chan) { return source.getSampleOrZero (chan, frame); });
}

choc::value::Value convertChannelSetToArray (DiscreteChannelSet<const float> source)
{
    return createArrayFromChannelSet (source, source.numChannels);
}

choc::value::Value convertChannelSetToArray (DiscreteChannelSet<const float> source, uint32_t targetNumChannels)
{
    return createArrayFromChannelSet (source, targetNumChannels);
}

choc::value::ValueView getChannelSetAsArrayView (InterleavedChannelSet<float> source)
{
    return choc::value::ValueView::create2DArray (static_cast<float*> (source.data), source.numFrames, source.numChannels);
}

choc::value::ValueView getChannelSetAsArrayView (InterleavedChannelSet<const float> source)
{
    return choc::value::ValueView::create2DArray (const_cast<float*> (source.data), source.numFrames, source.numChannels);
}

InterleavedChannelSet<float> getChannelSetFromArray (const choc::value::ValueView& sourceArray)
{
    auto frameType = sourceArray.getType().getElementType();
    uint32_t numChannels = 1;

    if (frameType.isVector())
    {
        SOUL_ASSERT (frameType.getElementType().isFloat32());
        numChannels = frameType.getNumElements();
    }
    else
    {
        SOUL_ASSERT (frameType.isFloat32());
    }

    return { static_cast<float*> (const_cast<void*> (sourceArray.getRawData())), numChannels, sourceArray.size(), numChannels };
}

choc::value::Value createAudioDataObject (const choc::value::ValueView& frames, double sampleRate)
{
    auto o = choc::value::Value::createObject ("soul::AudioFile");
    o.addObjectMember ("frames", frames);
    o.addObjectMember ("sampleRate", sampleRate);
    return o;
}

choc::value::Value convertAudioDataToObject (InterleavedChannelSet<const float> source, double sampleRate)
{
    return createAudioDataObject (getChannelSetAsArrayView (source), sampleRate);
}

choc::value::Value convertAudioDataToObject (DiscreteChannelSet<const float> source, double sampleRate)
{
    return createAudioDataObject (convertChannelSetToArray (source), sampleRate);
}

choc::value::Value coerceAudioFileObjectToTargetType (const Type& targetType, const choc::value::ValueView& sourceValue)
{
    SOUL_ASSERT (sourceValue.isObject()); // this is only designed for use on objects which contain members for the frames + rate

    auto isFrameArray = [] (const choc::value::ValueView& member)
    {
        if (member.isArray())
        {
            auto isAudioSample = [] (const choc::value::ValueView& c) { return c.isInt32() || c.isFloat32() || c.isFloat64(); };

            auto first = member[0];

            return (first.isPrimitive() && isAudioSample (first))
                     || (first.isVector() && isAudioSample (first[0]));
        }

        return false;
    };

    auto isRateName = [] (const std::string& s)
    {
        return s == "rate" || s == "sampleRate" || s == "frequency";
    };

    choc::value::Value sourceFrameArray, sourceRate;

    for (uint32_t i = 0; i < sourceValue.size(); ++i)
    {
        auto member = sourceValue.getObjectMemberAt (i);

        if (isFrameArray (member.value))
            sourceFrameArray = member.value;
        else if (isRateName (member.name))
            sourceRate = member.value;
    }

    SOUL_ASSERT (! (sourceFrameArray.isVoid() || sourceRate.isVoid()));

    if (targetType.isArray())
        return sourceFrameArray;

    if (targetType.isStruct())
    {
        auto o = choc::value::Value::createObject ("soul::AudioSample");

        for (auto& m : targetType.getStructRef().getMembers())
        {
            if (m.type.isArray() && m.type.getArrayElementType().isPrimitiveOrVector())
                o.addObjectMember (m.name, sourceFrameArray);
            else if ((m.type.isFloatingPoint() || m.type.isPrimitiveInteger()) && isRateName (m.name))
                o.addObjectMember (m.name, sourceRate);
        }

        return o;
    }

    return choc::value::Value (sourceValue);
}

//==============================================================================
choc::value::Value generateWaveform (double frequency, double sampleRate, int64_t numFrames,
                                     WaveGenerators::Generator& generator,
                                     uint32_t oversamplingFactor)
{
    if (numFrames > 0 && frequency > 0 && sampleRate > 0 && numFrames < 48000 * 60 * 60 * 2)
    {
        AllocatedChannelSet<DiscreteChannelSet<float>> data (1, (uint32_t) (numFrames * oversamplingFactor));
        auto* samples = data.getChannel (0);

        generator.init (frequency, sampleRate * oversamplingFactor);

        for (uint32_t i = 0; i < data.getNumFrames(); ++i)
        {
            samples[i] = (float) generator.getSample();
            generator.advance();
        }

        if (oversamplingFactor == 1)
            return convertAudioDataToObject (data.channelSet, sampleRate);

        // Resample to the right size
        AllocatedChannelSet<DiscreteChannelSet<float>> resampledData (1, (uint32_t) (numFrames));
        resampleToFit (resampledData.channelSet, data.channelSet);
        return convertAudioDataToObject (resampledData.channelSet, sampleRate);
    }

    return {};
}

template <class Generator>
static choc::value::Value generateWaveform (const Annotation& annotation, uint32_t oversamplingFactor)
{
    Generator g;

    return generateWaveform (annotation.getDouble ("frequency"),
                             annotation.getDouble ("rate"),
                             annotation.getInt64 ("numFrames"),
                             g,
                             oversamplingFactor);
}

choc::value::Value generateWaveform (const Annotation& annotation)
{
    if (annotation.getBool ("sinewave") || annotation.getBool ("sine"))
        return generateWaveform<WaveGenerators::Sine> (annotation, 1);

    if (annotation.getBool ("sawtooth") || annotation.getBool ("saw"))
        return generateWaveform<WaveGenerators::Saw> (annotation, 2);

    if (annotation.getBool ("triangle"))
        return generateWaveform<WaveGenerators::Triangle> (annotation, 2);

    if (annotation.getBool ("squarewave") || annotation.getBool ("square"))
        return generateWaveform<WaveGenerators::Square> (annotation, 2);

    return {};
}

}
