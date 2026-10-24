Synthetic 440 Hz mono, 8 kHz, 100 ms tone. No recorded or third-party audio.
Generated with FFmpeg 8.0.1 for editor preview decoder regression tests.

ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=0.1" -ac 1 -map_metadata -1 -flags +bitexact -c:a pcm_s16le tone.wav
ffmpeg -i tone.wav -map_metadata -1 -flags +bitexact -c:a libmp3lame -b:a 32k tone.mp3
ffmpeg -i tone.wav -map_metadata -1 -flags +bitexact -c:a flac tone.flac
ffmpeg -i tone.wav -map_metadata -1 -flags +bitexact -c:a libvorbis tone.ogg
