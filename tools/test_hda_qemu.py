#!/usr/bin/env python3
"""Boot an isolated ReliefOS image with a selected QEMU audio topology.

Boot-only runs open the VMDK with snapshot=on. Probe runs use a private full
copy so guest capture files can be retained; the production VMDK is never
writable. The QMP socket stays in the worktree output directory. Serial is retained even
when the guest cannot reach userland, so controller/module diagnostics remain
reviewable.
"""
from __future__ import annotations
import argparse, array, hashlib, io, json, math, os, re, shutil, socket, struct, subprocess, time, wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def digest(path: Path) -> str:
    with path.open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()

def stage_probe(image: Path, out: Path, probe: Path, capture: bool, card: int,
                shm_probe: Path, pcm_backend: str, legacy: bool,
                standard_probe: Path | None = None,
                standard_doom_seconds: int = 5,
                standard_load_seconds: int = 0) -> Path:
    """Modify only a private conversion of the input image, keeping its GPT/ESP."""
    raw=out/'guest.raw'; root_image=out/'guest-root.ext4'
    with (out/'stage.log').open('x') as log:
        subprocess.run(['qemu-img','convert','-f','vmdk','-O','raw',str(image),str(raw)],check=True,stdout=log,stderr=log)
        with raw.open('rb') as disk:
            disk.seek(512); header=disk.read(92)
            if header[:8]!=b'EFI PART':raise ValueError('input has no GPT header')
            entries=struct.unpack_from('<Q',header,72)[0]; entry_size=struct.unpack_from('<I',header,84)[0]
            disk.seek(entries*512+entry_size); entry=disk.read(entry_size)
            first,last=struct.unpack_from('<QQ',entry,32); remaining=(last-first+1)*512
            if first>last or (last+1)*512>raw.stat().st_size:raise ValueError('invalid root partition')
            disk.seek(first*512)
            (out/'partition.json').write_text(json.dumps({'first_sector':first,'bytes':remaining})+'\n')
            with root_image.open('xb') as target:
                while remaining:
                    data=disk.read(min(8*1024*1024,remaining))
                    if not data:raise ValueError('truncated root partition')
                    target.write(data);remaining-=len(data)
        service=out/'reliefos-audio-qa'; script=out/'audio-qa.sh'
        service.write_text('#!/sbin/openrc-run\ndescription="ReliefOS audio QA"\n'
            'depend() { need reliefos-runtime; before reliefos-session; }\n'
            'start() { /bin/sh /usr/lib/reliefos/tests/audio-qa.sh > /dev/ttyS0 2>&1; }\n')
        commands=[['--mode','abi'],['--mode','play','--backend',pcm_backend]]
        if not legacy:
            commands.append(['--mode','timer'])
        if not legacy:commands += [['--mode','lifetime'],['--mode','controls']]
        if capture:commands.append(['--mode','capture','--frames','4096','--output','/usr/lib/reliefos/tests/audio-capture.raw'])
        script.write_text('#!/bin/sh\nfailures=0\nprintf "[audio-qa] BEGIN normal-openrc\n"\n'
            + '\n'.join('/usr/lib/reliefos/tests/audio-guest --card '+str(card)+' '+' '.join(args)+' || failures=$((failures + 1))' for args in commands)
            + '\n/usr/lib/reliefos/tests/sysv-shm-native || failures=$((failures + 1))\n'
            + '\n/bin/busybox sync || failures=$((failures + 1))\n'
            + 'printf "[audio-qa] DONE failures=%s\n" "$failures"\n')
        standard_service = out/'reliefos-standard-qa'
        standard_script = out/'standard-qa.sh'
        if standard_probe:
            standard_service.write_text('#!/sbin/openrc-run\ndescription="ReliefOS standard ALSA QA"\n'
                'depend() { need reliefos-runtime; after reliefos-audio-qa; before reliefos-session; }\n'
                'start() { failures=0; /bin/busybox sleep 2; /bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode abi > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                f'/bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode doom --doom-seconds {standard_doom_seconds} --load-seconds {standard_load_seconds} > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                '/bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode play > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                '/bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode capture > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                '/bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode lifetime > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                '/bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode concurrent > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                '/bin/sh /usr/lib/reliefos/tests/standard-probe.sh --mode controls > /dev/ttyS0 2>&1 || failures=$((failures + 1)); '
                'printf "[u6-standard] DONE failures=%s\\n" "${failures:-0}"; }\n')
            standard_script.write_text('#!/bin/sh\nexec /bin/sh /usr/lib/reliefos/tests/u6-standard-probe.sh "$@"\n')
        # debugfs edits the copied partition only; normal filesystem ownership,
        # kernel, loader, init and OpenRC binaries remain the input image's bytes.
        batch=out/'debugfs.commands'
        ops=['mkdir /usr/lib/reliefos/tests',
             f'write "{probe}" /usr/lib/reliefos/tests/audio-guest',
             'set_inode_field /usr/lib/reliefos/tests/audio-guest mode 0100755',
             f'write "{shm_probe}" /usr/lib/reliefos/tests/sysv-shm-native',
             'set_inode_field /usr/lib/reliefos/tests/sysv-shm-native mode 0100755',
             f'write "{script}" /usr/lib/reliefos/tests/audio-qa.sh',
             f'write "{service}" /etc/init.d/reliefos-audio-qa',
             'set_inode_field /etc/init.d/reliefos-audio-qa mode 0100755',
             'symlink /etc/runlevels/default/reliefos-audio-qa ../../init.d/reliefos-audio-qa']
        if standard_probe:
            ops += [f'write "{standard_probe}" /usr/lib/reliefos/tests/u6-standard-probe.sh',
                    'set_inode_field /usr/lib/reliefos/tests/u6-standard-probe.sh mode 0100755',
                    f'write "{standard_script}" /usr/lib/reliefos/tests/standard-probe.sh',
                    'set_inode_field /usr/lib/reliefos/tests/standard-probe.sh mode 0100755',
                    f'write "{standard_service}" /etc/init.d/reliefos-standard-qa',
                    'set_inode_field /etc/init.d/reliefos-standard-qa mode 0100755',
                    'symlink /etc/runlevels/default/reliefos-standard-qa ../../init.d/reliefos-standard-qa']
        batch.write_text('\n'.join(ops)+'\n')
        subprocess.run(['debugfs','-w','-f',str(batch),str(root_image)],check=True,stdout=log,stderr=log)
        with raw.open('r+b') as disk, root_image.open('rb') as partition:
            disk.seek(first*512);shutil.copyfileobj(partition,disk,8*1024*1024)
    return raw

def analyze_wave(path: Path, core_frames: int | None = 96000) -> dict:
    """Check the core stereo tone, including gaps and repetition.

    The native PCM timer probe and standard utilities intentionally play more
    audio after the initial two-second gate. Analyze that first segment by
    default; a standalone tone with idle padding can request the whole file.
    """
    raw=path.read_bytes(); finalized=True
    if len(raw) < 44:raise ValueError('truncated WAV container')
    if len(raw)>44 and raw[:4]==b'RIFF' and raw[8:16]==b'WAVEfmt ' and raw[36:40]==b'data' and struct.unpack_from('<I',raw,4)[0] in (0,36) and raw[40:44]==b'\0'*4:
        # QEMU may exit without finalizing its WAV sizes. Preserve its original
        # bytes and describe the condition; normalize only the container lengths
        # in a separate artifact, without adding/removing a single PCM sample.
        if struct.unpack_from('<IHHIIHH',raw,16)!=(16,1,2,48000,192000,4,16) or (len(raw)-44)%4:
            raise ValueError('invalid unfinalized QEMU WAV geometry')
        normalized=bytearray(raw)
        struct.pack_into('<I',normalized,4,len(raw)-8)
        struct.pack_into('<I',normalized,40,len(raw)-44)
        with (path.parent/'normalized-tone.wav').open('xb') as out:out.write(normalized)
        raw=bytes(normalized); finalized=False
    with wave.open(io.BytesIO(raw),'rb') as wav:
        rate=wav.getframerate();channels=wav.getnchannels();width=wav.getsampwidth()
        if channels!=2 or width!=2 or rate!=48000:raise ValueError('WAV must contain 48 kHz S16 stereo')
        frames=wav.getnframes() if core_frames is None else min(core_frames,wav.getnframes())
        if core_frames is not None and frames < core_frames:
            raise ValueError(f'WAV contains only {frames} core frames, expected {core_frames}')
        samples=array.array('h',wav.readframes(frames))
    if os.sys.byteorder!='little':samples.byteswap()
    active=[n for n in range(len(samples)//2) if max(abs(samples[2*n]),abs(samples[2*n+1]))>100]
    if not active:raise ValueError('WAV has no nonzero audio')
    duration=(active[-1]-active[0]+1)/rate
    if not 1.8<=duration<=2.3:raise ValueError(f'active tone duration {duration:.4f}s')
    start=(active[0]+active[-1])//2-4096; size=8192; metrics=[]
    for channel,target,opposite in [(0,1000,2000),(1,2000,1000)]:
        values=[samples[2*n+channel] for n in range(start,start+size)]
        def power(freq):
            step=2*math.pi*freq/rate
            re=sum(v*math.cos(step*n) for n,v in enumerate(values))
            im=sum(v*math.sin(step*n) for n,v in enumerate(values))
            return re*re+im*im
        peak=max(range(target-20,target+21),key=power)
        if abs(peak-target)>2 or power(target)<10*power(opposite):
            raise ValueError(f'channel {channel} unexpected frequency/crosstalk: {peak}')
        metrics.append({'channel':channel,'frequency_hz':peak,'peak_amplitude':max(map(abs,values))})
    # Both known tones share a 48-frame period. Compare every active frame
    # against a central period; an FFT of one middle window misses gaps and
    # repeated/skipped DMA descriptors elsewhere in the recording.
    reference=(active[0]+active[-1])//2
    maximum_error=0
    for n in range(active[0],active[-1]+1):
        expected=reference+(n-reference)%48
        for channel in (0,1):
            error=abs(samples[2*n+channel]-samples[2*expected+channel])
            maximum_error=max(maximum_error,error)
            if error>max(8,metrics[channel]['peak_amplitude']//50):
                raise ValueError(f'tone continuity mismatch at frame {n}, channel {channel}: {error}')
    span=active[-1]-active[0]+1
    if abs(span-96000)>48:
        raise ValueError(f'tone continuity duration mismatch: {span} frames, expected 96000')
    return {'sample_rate':rate,'channels':channels,'active_seconds':duration,'tones':metrics,
            'continuity_frames_tested':span,'max_period_error':maximum_error,
            'original_container_finalized':finalized}

def qmp_quit(path: Path, process: subprocess.Popen) -> None:
    try:
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(2); sock.connect(str(path)); sock.recv(65536)
            sock.sendall(b'{"execute":"qmp_capabilities"}\n'); sock.recv(65536)
            sock.sendall(b'{"execute":"quit"}\n')
        process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()

def file_audio_backend(out: Path, input_seconds: int = 60) -> tuple[list[str],dict]:
    """Supply enough controlled ADC input for the complete guest run."""
    period=b''.join(struct.pack('<hh',int(12000*math.sin(2*math.pi*n/120)),
                                 int(12000*math.sin(4*math.pi*n/120))) for n in range(120))
    input_seconds = max(1, int(input_seconds))
    # QEMU's file input advances while the guest device is active.  A fixed
    # 60-second source cannot cover every post-Doom capture check in a long
    # run. Keep known ADC data available without attributing a guest XRUN to
    # EOF alone. Write one second-sized block at a time to keep staging
    # memory bounded while covering the requested run plus settling time.
    with (out/'input.raw').open('xb') as source:
        block = period * (48000 // 120)
        for _ in range(input_seconds):
            source.write(block)
    config=out/'alsa.conf'
    config.write_text('pcm.qa_null { type null }\n'
        'pcm.qa_output { type file slave.pcm "qa_null" '
        f'file "{out}/tone.wav" format "wav" truncate false }}\n'
        'pcm.qa_input { type file slave.pcm "qa_null" '
        f'infile "{out}/input.raw" file "{out}/input-monitor.raw" format "raw" truncate false }}\n')
    env=os.environ.copy();env['ALSA_CONFIG_PATH']=str(config)
    args=['-audiodev','alsa,id=snd0,out.dev=qa_output,in.dev=qa_input,'
          'out.try-poll=off,in.try-poll=off,out.frequency=48000,in.frequency=48000,'
          'out.channels=2,in.channels=2,out.format=s16,in.format=s16']
    return args,env

def extract_capture(raw: Path,out: Path) -> Path:
    """Extract guest-written data from the owned disk after QEMU closes it."""
    layout=json.loads((out/'partition.json').read_text()); root=out/'captured-root.ext4'
    with raw.open('rb') as disk,root.open('xb') as part:
        disk.seek(layout['first_sector']*512);remaining=layout['bytes']
        while remaining:
            data=disk.read(min(8*1024*1024,remaining))
            if not data:raise ValueError('truncated guest disk')
            part.write(data);remaining-=len(data)
    capture=out/'captured.raw'
    with (out/'capture-extract.log').open('x') as log:
        subprocess.run(['debugfs','-R',f'dump /usr/lib/reliefos/tests/audio-capture.raw {capture}',str(root)],
                       check=True,stdout=log,stderr=log)
    return capture

def verify_capture(path: Path) -> dict:
    """Match guest ADC samples to the distinct injected 400/800 Hz sequence."""
    raw=path.read_bytes()
    if len(raw)!=4096*4:raise ValueError(f'capture byte count {len(raw)} != 16384')
    frames=list(struct.iter_unpack('<hh',raw))
    first=next((n for n,frame in enumerate(frames) if max(map(abs,frame))>100),len(frames))
    if first>1024:raise ValueError('capture lacks enough nonzero input samples')
    pattern=[(int(12000*math.sin(2*math.pi*n/120)),int(12000*math.sin(4*math.pi*n/120))) for n in range(120)]
    for phase in range(120):
        if all(max(abs(frame[c]-pattern[(n+phase)%120][c]) for c in (0,1))<=2
               for n,frame in enumerate(frames[first:])):
            return {'frames':len(frames),'bytes':len(raw),'rate':48000,'channels':2,
                    'input_tones_hz':[400,800],'leading_silence_frames':first,
                    'matched_frames':len(frames)-first,'sample_tolerance':2,'sha256':digest(path)}
    raise ValueError('ADC data does not match the injected stereo sequence')

def main() -> int:
    p=argparse.ArgumentParser()
    p.add_argument('--image',type=Path,required=True); p.add_argument('--output',type=Path,required=True)
    p.add_argument('--controller',choices=['intel-hda','ich9-intel-hda','ac97','es1371','none'],default='intel-hda')
    p.add_argument('--codec',choices=['hda-duplex','hda-output'],default='hda-duplex')
    p.add_argument('--smp',type=int,choices=[1,4],default=4); p.add_argument('--msi',choices=['on','off'],default='on')
    p.add_argument('--machine',choices=['pc','q35'],default='q35'); p.add_argument('--timeout',type=float,default=90)
    p.add_argument('--boot-only',action='store_true',help='collect module/boot evidence without claiming guest audio')
    p.add_argument('--dual-codec',action='store_true')
    p.add_argument('--audio-backend',choices=['wav','alsa-file'],default='wav')
    p.add_argument('--pcm-backend',choices=['alsa','oss'],help='defaults to OSS for legacy devices, ALSA for HDA')
    p.add_argument('--card',type=int,choices=range(16),default=0)
    p.add_argument('--shm-probe',type=Path,default=ROOT/'out/audio-hda/qa/sysv-shm-native-19-final')
    p.add_argument('--probe',type=Path,default=ROOT/'out/audio-hda/qa/audio-guest-accepted')
    p.add_argument('--standard-probe',type=Path,
                   help='stage the shell-based ALSA utility probe after the core QA service')
    p.add_argument('--standard-doom-seconds',type=int,default=5,
                   help='headless Doom duration for the standard probe')
    p.add_argument('--standard-load-seconds',type=int,default=0,
                   help='CPU/I/O load duration during the standard Doom probe')
    a=p.parse_args(); image=a.image.resolve(); out=a.output.resolve()
    if not image.is_file(): p.error(f'missing image: {image}')
    if a.standard_doom_seconds <= 0: p.error('standard Doom duration must be positive')
    if a.standard_load_seconds < 0: p.error('standard load duration cannot be negative')
    if not out.is_relative_to(ROOT/'out') or out==ROOT/'out':p.error('output must be a fresh worktree out subdirectory')
    if out.exists():p.error('output exists; choose a new directory to preserve evidence')
    if len(str(out/'qmp.sock').encode())>=108:p.error('output path is too long for a Unix QMP socket')
    out.mkdir(parents=True); serial=out/'serial.log'; qmp=out/'qmp.sock'; qlog=out/'qemu.log'
    original_hash=digest(image); probe=a.probe.resolve(); boot_image=image; image_format='vmdk'
    legacy=a.controller in ('ac97','es1371')
    pcm_backend=a.pcm_backend or ('oss' if legacy else 'alsa')
    capture=not legacy and a.controller!='none' and a.codec=='hda-duplex' and a.audio_backend=='alsa-file'
    if not a.boot_only:
        if not probe.is_file():p.error(f'missing guest probe: {probe}')
        shm_probe=a.shm_probe.resolve()
        if not shm_probe.is_file():p.error(f'missing native SHM probe: {shm_probe}')
        boot_image=stage_probe(image,out,probe,capture,a.card,shm_probe,pcm_backend,legacy,
                               a.standard_probe.resolve() if a.standard_probe else None,
                               a.standard_doom_seconds, a.standard_load_seconds);image_format='raw'
    tone=out/'tone.wav'
    sound=['-audiodev',f'wav,id=snd0,path={tone},out.frequency=48000,out.channels=2,out.format=s16']
    qemu_env=None
    # Cover repeated capture lifetimes and a 60-second streamed-file check
    # after Doom, without mistaking controlled-input EOF for valid silence.
    audio_input_seconds = max(180, a.standard_doom_seconds + 180)
    if a.audio_backend=='alsa-file':sound,qemu_env=file_audio_backend(out, audio_input_seconds)
    if a.controller in ('intel-hda','ich9-intel-hda'):
        sound += ['-device',f'{a.controller},id=hda0,msi={a.msi}', '-device',f'{a.codec},audiodev=snd0']
        if a.dual_codec:sound+=['-device',f'{a.codec},audiodev=snd0']
    elif a.controller=='ac97': sound += ['-device','AC97,audiodev=snd0']
    elif a.controller=='es1371': sound += ['-device','ES1370,audiodev=snd0']
    accel=['-enable-kvm','-cpu','host'] if os.access('/dev/kvm',os.R_OK|os.W_OK) else ['-cpu','max']
    cmd=['qemu-system-x86_64','-machine',a.machine,*accel,'-m','2048M','-smp',str(a.smp),
         '-bios','/usr/share/edk2/x64/OVMF.4m.fd','-display','none','-serial',f'file:{serial}',
         '-drive',f'file={boot_image},if=none,id=disk0,format={image_format},snapshot={"on" if a.boot_only else "off"}','-device','ich9-ahci,id=ahci',
         '-device','ide-hd,drive=disk0,bus=ahci.0','-qmp',f'unix:{qmp},server=on,wait=off',
         '-no-reboot','-no-shutdown']+sound
    (out/'run.json').write_text(json.dumps({'command':cmd,'image':str(image),'image_sha256':original_hash,
        'boot_image':str(boot_image),'snapshot':a.boot_only,'private_full_copy':not a.boot_only,
        'audio_backend':a.audio_backend,'pcm_backend':pcm_backend,
        'capture_enabled':capture and not a.boot_only,
        'probe_sha256':None if a.boot_only else digest(probe),
        'runner_sha256':digest(Path(__file__)),'card_ordinal':a.card,
        'shm_probe_sha256':None if a.boot_only else digest(shm_probe),
        'controller':a.controller,'codec':a.codec,'smp':a.smp,'msi':a.msi,'machine':a.machine,
        'standard_doom_seconds':a.standard_doom_seconds,
        'standard_load_seconds':a.standard_load_seconds,
        'audio_input_seconds':audio_input_seconds},indent=2)+'\n')
    with qlog.open('w') as log: proc=subprocess.Popen(cmd,cwd=ROOT,env=qemu_env,stdout=log,stderr=log)
    deadline=time.monotonic()+a.timeout
    done_marker = '[u6-standard] DONE failures=' if a.standard_probe else '[audio-qa] DONE failures='
    try:
        while proc.poll() is None and time.monotonic()<deadline:
            time.sleep(.25)
            text=serial.read_text(errors='replace') if serial.exists() else ''
            if done_marker in text:break
            if a.boot_only and '[reliefnt] boot complete:' in text:break
    finally:
        if proc.poll() is None:qmp_quit(qmp,proc)
    text=serial.read_text(errors='replace') if serial.exists() else ''
    lines=[x for x in text.splitlines() if '[hda]' in x or '[driver]' in x or 'boot complete' in x or '[audio-qa]' in x or '[u6-standard]' in x or 'PASS ' in x or 'FAIL ' in x]
    (out/'hda-lines.log').write_text('\n'.join(lines)+'\n')
    ready='[hda] registered usable codec card' in text
    waveform=False;wave_error=None;metrics=None
    if not a.boot_only and a.controller!='none':
        try:metrics=analyze_wave(tone, None if legacy else 96000);waveform=True
        except (OSError,ValueError,wave.Error,EOFError) as exc:wave_error=str(exc) or type(exc).__name__
    expected=ready if 'hda' in a.controller else (f'loaded {a.controller} abi=' in text if a.controller!='none' else not ready)
    shm_match=re.search(r'PASS native SysV SHM checks=(\d+); all owned segments removed',text)
    shm_checks=int(shm_match.group(1)) if shm_match else 0
    capture_verified=False;capture_metrics=None;capture_error=None
    if not a.boot_only and capture:
        try:capture_metrics=verify_capture(extract_capture(boot_image,out));capture_verified=True
        except (OSError,ValueError,subprocess.SubprocessError) as exc:capture_error=str(exc)
    standard_pass = not a.standard_probe or '[u6-standard] DONE failures=0' in text
    result={'boot_complete':'[reliefnt] boot complete:' in text,'hda_card_registered':ready,
            'expected_module_state':expected,'waveform_verified':waveform,'wave_error':wave_error,
            'wave_metrics':metrics,'capture_data_verified':capture_verified,
            'capture_metrics':capture_metrics,'capture_error':capture_error,'qemu_exit':proc.returncode,
            'capture_syscall_verified':'PASS capture_frames=' in text,'serial':str(serial),
            'pcm_backend':pcm_backend,'capture_enabled':capture and not a.boot_only,
            'legacy_ALSA_validated':False if legacy else None,
            'guest_probe': 'boot-only' if a.boot_only else 'normal-openrc standard-libc',
            'guest_probe_pass':'[audio-qa] DONE failures=0' in text and standard_pass,
            'shm_guest_verified':shm_checks>=82,'shm_guest_checks':shm_checks,
            'original_image_unchanged':digest(image)==original_hash}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n'); print('\n'.join(lines))
    print(json.dumps(result,sort_keys=True))
    passed=result['boot_complete'] and expected and result['original_image_unchanged']
    if not a.boot_only:passed=passed and result['guest_probe_pass'] and waveform and result['shm_guest_verified']
    if not a.boot_only and capture:passed=passed and capture_verified
    return 0 if passed else 1
if __name__=='__main__': raise SystemExit(main())
