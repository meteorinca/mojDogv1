import asyncio
import edge_tts
import subprocess
import os

async def gen():
    text = "Hi, my name is Paulbot."
    voice = "en-US-GuyNeural"
    temp_mp3 = "temp_boot.mp3"
    print("Generating TTS...")
    communicate = edge_tts.Communicate(text, voice)
    await communicate.save(temp_mp3)
    
    command = [
        'ffmpeg', '-i', temp_mp3, '-f', 's16le', '-acodec', 'pcm_s16le',
        '-ar', '16000', '-ac', '1', '-'
    ]
    print("Converting...")
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
    raw_pcm_data = result.stdout
    
    with open('main/paulbot_audio.h', 'w') as f:
        f.write('#ifndef PAULBOT_AUDIO_H\n#define PAULBOT_AUDIO_H\n\n')
        f.write('const uint8_t paulbot_audio[] = {\n')
        f.write(', '.join([f'0x{b:02x}' for b in raw_pcm_data]))
        f.write('\n};\n')
        f.write(f'const size_t paulbot_audio_len = {len(raw_pcm_data)};\n')
        f.write('\n#endif\n')
    
    os.remove(temp_mp3)
    print("Done generating boot audio!")

if __name__ == "__main__":
    asyncio.run(gen())
