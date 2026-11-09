"""The environment of every test run of a game exe (tools/boot/boot.py, tools/test/run_suite.py): headless, 720p, no pool mirror, a muted
sink with a real clock. A caller applies it under the process environment and the run's own settings."""


def testrun_env():
    return {
        'NXRT_BOOT_GFX': 'headless',            # the game exe opens a play-mode window by default
        'NXRT_BOOT_HEADLESS_WIDTH': '1280', 'NXRT_BOOT_HEADLESS_HEIGHT': '720',
        'NXRT_NVN_POOL_MIRROR': 'off',
        'NXRT_AUDIO_SINK': 'paced',             # muted; only an explicit audio test uses NXRT_AUDIO_SINK=xaudio2
    }
