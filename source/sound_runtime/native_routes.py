"""Add compatibility guards for the runtime's native ownership boundaries.

Admission follows Journey's protected worker, retirement only invalidates
identity, and Tone/Stream publication precedes their original unpause. The
original callback and userdata contracts remain intact. The hook-contract table
selects the sites; this module only registers required imports and native callback
fingerprints with the patcher, not new hooks.
"""

def configure(patcher):
    if getattr(patcher, '_native_routes_configured', False):
        return
    patcher._native_routes_configured = True
    patcher.EXPECTED_IMPORTS.update({5583456: ('KERNEL32.dll', 'ReleaseSemaphore'), 5586584: ('fmod64.dll', '?setCallback@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW43@PEAUFMOD_CHANNELCONTROL@@W4FMOD_CHANNELCONTROL_TYPE@@W4FMOD_CHANNELCONTROL_CALLBACK_TYPE@@PEAX3@Z@Z'), 5586704: ('fmod64.dll', '?setPaused@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z'), 5586976: ('fmod64.dll', '?stop@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@XZ')})
    patcher.EXPECTED_FIXED_TARGETS.update({rva:bytes.fromhex(raw) for rva,raw in {2880288: '40534883ec30488bd985d2752a4585c07525488d54242048c744242000000000', 2901360: '48895c24084889742410574883ec30498bd9418bf8488bf185d20f85a9000000'}.items()})
