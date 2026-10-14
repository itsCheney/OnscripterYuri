/*
  native_midi_ios: native MIDI for SDL_mixer on iOS, for Mikage.
  Based on native_midi_macosx.c, Copyright (C) 2009 Ryan C. Gordon
  <icculus@icculus.org>. Altered for iOS by the Mikage project: iOS has no
  built-in General MIDI instruments, so each song plays through its own
  AUGraph (MIDISynth -> mixer -> RemoteIO) with the SoundFont set by
  Mix_SetSoundFonts().

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_stdinc.h"

#include <AudioToolbox/AudioToolbox.h>

#include "SDL_mixer.h"
#include "native_midi.h"

struct _NativeMidiSong
{
    MusicPlayer player;
    MusicSequence sequence;
    MusicTimeStamp endTime;
    AUGraph graph;
    AudioUnit synth;
    AudioUnit mixer;
    MusicTimeStamp pausedAt;
    SDL_bool paused;
    int loops;
};

static NativeMidiSong *currentsong = NULL;
static int latched_volume = MIX_MAX_VOLUME;

static OSStatus GetSequenceLength(MusicSequence sequence, MusicTimeStamp *length)
{
    UInt32 ntracks, i;
    MusicTimeStamp longest = 0;
    OSStatus err = MusicSequenceGetTrackCount(sequence, &ntracks);
    if (err != noErr)
        return err;
    for (i = 0; i < ntracks; ++i) {
        MusicTrack track;
        MusicTimeStamp tracklen = 0;
        UInt32 size = sizeof(tracklen);
        err = MusicSequenceGetIndTrack(sequence, i, &track);
        if (err != noErr)
            return err;
        err = MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength, &tracklen, &size);
        if (err != noErr)
            return err;
        if (longest < tracklen)
            longest = tracklen;
    }
    *length = longest;
    return noErr;
}

static int SDLCALL LoadSoundFont(const char *path, void *data)
{
    AudioUnit synth = *(AudioUnit *)data;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                           (const UInt8 *)path,
                                                           (CFIndex)SDL_strlen(path), false);
    OSStatus err;
    if (!url)
        return SDL_FALSE;
    err = AudioUnitSetProperty(synth, kMusicDeviceProperty_SoundBankURL,
                               kAudioUnitScope_Global, 0, &url, sizeof(url));
    CFRelease(url);
    return err == noErr ? SDL_TRUE : SDL_FALSE;
}

static OSStatus CreateGraph(NativeMidiSong *song)
{
    AudioComponentDescription synthDesc = {
        kAudioUnitType_MusicDevice, kAudioUnitSubType_MIDISynth, kAudioUnitManufacturer_Apple, 0, 0
    };
    AudioComponentDescription mixerDesc = {
        kAudioUnitType_Mixer, kAudioUnitSubType_MultiChannelMixer, kAudioUnitManufacturer_Apple, 0, 0
    };
    AudioComponentDescription outputDesc = {
        kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0
    };
    AUNode synthNode, mixerNode, outputNode;
    UInt32 buses = 1;
    OSStatus err;

    if ((err = NewAUGraph(&song->graph)) != noErr)
        return err;
    if ((err = AUGraphAddNode(song->graph, &synthDesc, &synthNode)) != noErr ||
        (err = AUGraphAddNode(song->graph, &mixerDesc, &mixerNode)) != noErr ||
        (err = AUGraphAddNode(song->graph, &outputDesc, &outputNode)) != noErr)
        return err;
    if ((err = AUGraphConnectNodeInput(song->graph, synthNode, 0, mixerNode, 0)) != noErr ||
        (err = AUGraphConnectNodeInput(song->graph, mixerNode, 0, outputNode, 0)) != noErr)
        return err;
    if ((err = AUGraphOpen(song->graph)) != noErr)
        return err;
    if ((err = AUGraphNodeInfo(song->graph, synthNode, NULL, &song->synth)) != noErr ||
        (err = AUGraphNodeInfo(song->graph, mixerNode, NULL, &song->mixer)) != noErr)
        return err;
    if ((err = AudioUnitSetProperty(song->mixer, kAudioUnitProperty_ElementCount,
                                    kAudioUnitScope_Input, 0, &buses, sizeof(buses))) != noErr)
        return err;
    /* Without a SoundFont the synth plays sine waves, so a missing one is
       still playable. */
    Mix_EachSoundFont(LoadSoundFont, &song->synth);
    if ((err = AUGraphInitialize(song->graph)) != noErr)
        return err;
    return MusicSequenceSetAUGraph(song->sequence, song->graph);
}

static void SilenceSynth(NativeMidiSong *song)
{
    UInt32 channel;
    if (!song->synth)
        return;
    for (channel = 0; channel < 16; ++channel) {
        MusicDeviceMIDIEvent(song->synth, 0xB0 | channel, 123, 0, 0); /* all notes off */
        MusicDeviceMIDIEvent(song->synth, 0xB0 | channel, 120, 0, 0); /* all sound off */
    }
}

static void ApplyVolume(NativeMidiSong *song)
{
    if (song && song->mixer) {
        AudioUnitSetParameter(song->mixer, kMultiChannelMixerParam_Volume, kAudioUnitScope_Output, 0,
                              (AudioUnitParameterValue)latched_volume / MIX_MAX_VOLUME, 0);
    }
}

int native_midi_detect(void)
{
    return 1;
}

NativeMidiSong *native_midi_loadsong_RW(SDL_RWops *src, int freesrc)
{
    NativeMidiSong *song = NULL;
    void *buf = NULL;
    CFDataRef data = NULL;
    Sint64 len;

    if (SDL_RWseek(src, 0, RW_SEEK_END) < 0 || (len = SDL_RWtell(src)) <= 0 ||
        SDL_RWseek(src, 0, RW_SEEK_SET) < 0)
        goto fail;
    if (!(buf = SDL_malloc((size_t)len)) || SDL_RWread(src, buf, (size_t)len, 1) != 1)
        goto fail;
    if (!(song = SDL_calloc(1, sizeof(*song))))
        goto fail;
    if (NewMusicPlayer(&song->player) != noErr || NewMusicSequence(&song->sequence) != noErr)
        goto fail;
    if (!(data = CFDataCreate(NULL, (const UInt8 *)buf, (CFIndex)len)))
        goto fail;
    SDL_free(buf);
    buf = NULL;
    if (MusicSequenceFileLoadData(song->sequence, data, 0, 0) != noErr)
        goto fail;
    CFRelease(data);
    data = NULL;
    if (GetSequenceLength(song->sequence, &song->endTime) != noErr ||
        CreateGraph(song) != noErr ||
        MusicPlayerSetSequence(song->player, song->sequence) != noErr)
        goto fail;
    if (freesrc)
        SDL_RWclose(src);
    return song;

fail:
    if (song)
        native_midi_freesong(song);
    if (data)
        CFRelease(data);
    SDL_free(buf);
    SDL_SetError("Couldn't load the MIDI song");
    return NULL;
}

void native_midi_freesong(NativeMidiSong *song)
{
    if (!song)
        return;
    if (currentsong == song)
        currentsong = NULL;
    if (song->player) {
        MusicPlayerStop(song->player);
        MusicPlayerSetSequence(song->player, NULL);
    }
    SilenceSynth(song);
    if (song->sequence)
        MusicSequenceSetAUGraph(song->sequence, NULL);
    if (song->graph) {
        AUGraphStop(song->graph);
        AUGraphUninitialize(song->graph);
        AUGraphClose(song->graph);
        DisposeAUGraph(song->graph);
    }
    if (song->sequence)
        DisposeMusicSequence(song->sequence);
    if (song->player)
        DisposeMusicPlayer(song->player);
    SDL_free(song);
}

void native_midi_start(NativeMidiSong *song, int loops)
{
    if (!song)
        return;
    if (currentsong && currentsong != song) {
        MusicPlayerStop(currentsong->player);
        SilenceSynth(currentsong);
    }
    currentsong = song;
    song->loops = loops;
    song->paused = SDL_FALSE;
    ApplyVolume(song);
    MusicPlayerSetTime(song->player, 0);
    MusicPlayerPreroll(song->player);
    MusicPlayerStart(song->player);
}

void native_midi_pause(void)
{
    if (!currentsong || currentsong->paused)
        return;
    MusicPlayerGetTime(currentsong->player, &currentsong->pausedAt);
    MusicPlayerStop(currentsong->player);
    SilenceSynth(currentsong);
    AUGraphStop(currentsong->graph);
    currentsong->paused = SDL_TRUE;
}

void native_midi_resume(void)
{
    if (!currentsong || !currentsong->paused)
        return;
    currentsong->paused = SDL_FALSE;
    AUGraphStart(currentsong->graph);
    MusicPlayerSetTime(currentsong->player, currentsong->pausedAt);
    MusicPlayerStart(currentsong->player);
}

void native_midi_stop(void)
{
    if (!currentsong)
        return;
    MusicPlayerStop(currentsong->player);
    SilenceSynth(currentsong);
    currentsong = NULL;
}

int native_midi_active(void)
{
    MusicTimeStamp now = 0;
    if (!currentsong)
        return 0;
    if (currentsong->paused)
        return 1;
    MusicPlayerGetTime(currentsong->player, &now);
    if (now < currentsong->endTime || now >= kMusicTimeStamp_EndOfTrack)
        return 1;
    if (currentsong->loops) {
        if (currentsong->loops > 0)
            --currentsong->loops;
        MusicPlayerSetTime(currentsong->player, 0);
        return 1;
    }
    return 0;
}

void native_midi_setvolume(int volume)
{
    latched_volume = volume;
    ApplyVolume(currentsong);
}

const char *native_midi_error(void)
{
    return "";
}
