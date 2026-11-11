// lkit_measure – minimal AU host for black-box measurement (LOMAS Plugin-kit; from Passive EQ).
//
// Only does what needs an Audio Unit: find it, describe/probe its parameters,
// and render a stimulus through it for a list of settings. Sweep generation and
// analysis live in Python (tools/measure/*.py) so that offline core renders can
// share the same analysis.
//
// Usage:
//   lkit_measure list [filter]
//   lkit_measure params --plugin SPEC [--json FILE]      (alias: --list-params)
//   lkit_measure probe  --plugin SPEC --param ID --values v1,v2,... [--json FILE]
//   lkit_measure render --plugin SPEC --job JOB.json --out DIR
//     job keys: sample_rate, block_size, settle_s, preroll_s, tail_s, stimulus, base_params, runs[{name, params,
//     gain, change, meta}], watch [ids: output parameters (meters) read once per recorded block -> NAME.json "watch"]
//
// Effects of type aufx (effect) and aumf (music effect) are both searched.
//
// SPEC is one of
//   a name substring ("PuigTec EQP1A (s)"; "Stereo"/"Mono" map to "(s)"/"(m)"),
//   type:subtype:manufacturer four-char codes ("aufx:P1AS:ksWV"),
//   a path to a .component bundle (our own builds; registered in-process).

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <string>
#include <vector>

static void check(OSStatus s, const char* what)
{
    if (s != noErr) {
        fprintf(stderr, "error: %s failed (OSStatus %d)\n", what, (int)s);
        exit(1);
    }
}

static OSType fourcc(NSString* s)
{
    const char* c = s.UTF8String;
    if (strlen(c) != 4) return 0;
    return ((OSType)c[0] << 24) | ((OSType)c[1] << 16) | ((OSType)c[2] << 8) | (OSType)c[3];
}

static std::string fourccString(OSType t)
{
    char c[5] = {(char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0};
    return c;
}

static NSString* componentName(AudioComponent c)
{
    CFStringRef name = nullptr;
    if (AudioComponentCopyName(c, &name) != noErr || !name) return @"";
    return CFBridgingRelease(name);
}

static NSString* argValue(int argc, char** argv, const char* key)
{
    for (int i = 2; i + 1 < argc; ++i)
        if (!strcmp(argv[i], key)) return @(argv[i + 1]);
    return nil;
}

// Register a .component bundle in this process and return its description.
static bool registerBundle(NSString* path, AudioComponentDescription& out)
{
    NSBundle* bundle = [NSBundle bundleWithPath:path];
    if (!bundle) { fprintf(stderr, "error: cannot open bundle %s\n", path.UTF8String); return false; }
    NSArray* comps = [bundle objectForInfoDictionaryKey:@"AudioComponents"];
    NSDictionary* comp = comps.firstObject;
    if (!comp) { fprintf(stderr, "error: no AudioComponents in Info.plist\n"); return false; }
    NSString* exe = bundle.executablePath;
    void* lib = dlopen(exe.UTF8String, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "error: %s\n", dlerror()); return false; }
    auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
        dlsym(lib, [comp[@"factoryFunction"] UTF8String]));
    if (!factory) { fprintf(stderr, "error: AU factory not found\n"); return false; }
    out = {};
    out.componentType = fourcc(comp[@"type"]);
    out.componentSubType = fourcc(comp[@"subtype"]);
    out.componentManufacturer = fourcc(comp[@"manufacturer"]);
    NSString* name = [NSString stringWithFormat:@"%@ (local bundle)", comp[@"name"]];
    UInt32 version = [comp[@"version"] unsignedIntValue];
    return AudioComponentRegister(&out, (__bridge CFStringRef)name, version, factory) != nullptr;
}

static AudioComponent findComponent(NSString* spec)
{
    AudioComponentDescription d = {};
    if ([spec hasSuffix:@".component"] || [spec hasSuffix:@".component/"]) {
        if (!registerBundle(spec, d)) return nullptr;
        // Our in-process registration is preferred over a system-installed copy.
        AudioComponent c = nullptr, last = nullptr;
        while ((c = AudioComponentFindNext(c, &d))) last = c;
        return last;
    }
    NSArray* parts = [spec componentsSeparatedByString:@":"];
    if (parts.count == 3 && [parts[0] length] == 4) {
        d.componentType = fourcc(parts[0]);
        d.componentSubType = fourcc(parts[1]);
        d.componentManufacturer = fourcc(parts[2]);
        return AudioComponentFindNext(nullptr, &d);
    }
    NSString* want = [[spec stringByReplacingOccurrencesOfString:@" Stereo" withString:@" (s)"]
        stringByReplacingOccurrencesOfString:@" Mono" withString:@" (m)"];
    for (OSType t : {kAudioUnitType_Effect, kAudioUnitType_MusicEffect}) {
        d.componentType = t;
        AudioComponent c = nullptr;
        while ((c = AudioComponentFindNext(c, &d)))
            if ([componentName(c) localizedCaseInsensitiveContainsString:want]) return c;
    }
    return nullptr;
}

struct Host {
    AudioComponent comp = nullptr;
    AudioUnit unit = nullptr;
    NSString* name = @"";
    UInt32 version = 0;
    AudioComponentDescription desc = {};
};

static Host openHost(NSString* spec)
{
    Host h;
    h.comp = findComponent(spec);
    if (!h.comp) {
        fprintf(stderr, "error: Audio Unit not found in this session: %s\n", spec.UTF8String);
        exit(3);
    }
    h.name = componentName(h.comp);
    AudioComponentGetVersion(h.comp, &h.version);
    AudioComponentGetDescription(h.comp, &h.desc);
    check(AudioComponentInstanceNew(h.comp, &h.unit), "instantiate");
    return h;
}

static std::vector<AudioUnitParameterID> paramIds(AudioUnit u)
{
    UInt32 size = 0;
    if (AudioUnitGetPropertyInfo(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
                                 &size, nullptr) != noErr)
        return {};
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    check(AudioUnitGetProperty(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
                               ids.data(), &size),
          "parameter list");
    return ids;
}

static NSString* paramString(AudioUnit u, AudioUnitParameterID id, float value)
{
    AudioUnitParameterStringFromValue sv = {};
    sv.inParamID = id;
    Float32 v = value;
    sv.inValue = &v;
    UInt32 size = sizeof(sv);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global,
                             0, &sv, &size) == noErr && sv.outString)
        return CFBridgingRelease(sv.outString);
    return nil;
}

static NSArray* flagNames(UInt32 f)
{
    const std::pair<UInt32, const char*> names[] = {
        {kAudioUnitParameterFlag_CFNameRelease, "CFNameRelease"},
        {kAudioUnitParameterFlag_OmitFromPresets, "OmitFromPresets"},
        {kAudioUnitParameterFlag_PlotHistory, "PlotHistory"},
        {kAudioUnitParameterFlag_MeterReadOnly, "MeterReadOnly"},
        {kAudioUnitParameterFlag_DisplaySquareRoot, "DisplaySquareRoot"},
        {kAudioUnitParameterFlag_DisplaySquared, "DisplaySquared"},
        {kAudioUnitParameterFlag_DisplayCubed, "DisplayCubed"},
        {kAudioUnitParameterFlag_DisplayCubeRoot, "DisplayCubeRoot"},
        {kAudioUnitParameterFlag_DisplayExponential, "DisplayExponential"},
        {kAudioUnitParameterFlag_HasClump, "HasClump"},
        {kAudioUnitParameterFlag_ValuesHaveStrings, "ValuesHaveStrings"},
        {kAudioUnitParameterFlag_DisplayLogarithmic, "DisplayLogarithmic"},
        {kAudioUnitParameterFlag_IsHighResolution, "IsHighResolution"},
        {kAudioUnitParameterFlag_NonRealTime, "NonRealTime"},
        {kAudioUnitParameterFlag_CanRamp, "CanRamp"},
        {kAudioUnitParameterFlag_ExpertMode, "ExpertMode"},
        {kAudioUnitParameterFlag_HasCFNameString, "HasCFNameString"},
        {kAudioUnitParameterFlag_IsGlobalMeta, "IsGlobalMeta"},
        {kAudioUnitParameterFlag_IsElementMeta, "IsElementMeta"},
        {kAudioUnitParameterFlag_IsReadable, "IsReadable"},
        {kAudioUnitParameterFlag_IsWritable, "IsWritable"},
    };
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& [bit, name] : names)
        if (f & bit) [a addObject:@(name)];
    return a;
}

// Unit-level properties: factory presets, channel configurations, latency, tail.
static NSDictionary* unitInfo(AudioUnit u)
{
    NSMutableDictionary* d = [NSMutableDictionary dictionary];
    CFArrayRef presets = nullptr;
    UInt32 size = sizeof(presets);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size) == noErr &&
        presets) {
        NSMutableArray* names = [NSMutableArray array];
        for (CFIndex i = 0; i < CFArrayGetCount(presets); ++i) {
            auto* p = static_cast<const AUPreset*>(CFArrayGetValueAtIndex(presets, i));
            [names addObject:@{@"number" : @(p->presetNumber), @"name" : (__bridge NSString*)p->presetName ?: @""}];
        }
        d[@"factory_presets"] = names;
        CFRelease(presets);
    }
    if (AudioUnitGetPropertyInfo(u, kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0, &size, nullptr) ==
        noErr && size > 0) {
        std::vector<AUChannelInfo> ci(size / sizeof(AUChannelInfo));
        if (AudioUnitGetProperty(u, kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0, ci.data(), &size) ==
            noErr) {
            NSMutableArray* a = [NSMutableArray array];
            for (const auto& c : ci) [a addObject:@[ @(c.inChannels), @(c.outChannels) ]];
            d[@"channel_configs"] = a;
        }
    }
    Float64 v = 0;
    size = sizeof(v);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &v, &size) == noErr)
        d[@"latency_s"] = @(v);
    size = sizeof(v);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &v, &size) == noErr)
        d[@"tail_s"] = @(v);
    UInt32 bypass = 0;
    size = sizeof(bypass);
    d[@"supports_bypass_property"] =
        @(AudioUnitGetProperty(u, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &bypass, &size) == noErr);
    return d;
}

static NSDictionary* paramInfo(AudioUnit u, AudioUnitParameterID id)
{
    AudioUnitParameterInfo p = {};
    UInt32 size = sizeof(p);
    check(AudioUnitGetProperty(u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &p,
                               &size),
          "parameter info");
    NSString* name = p.cfNameString ? (__bridge NSString*)p.cfNameString : @(p.name);
    NSMutableDictionary* d = [@{
        @"id" : @(id),
        @"name" : name ?: @"",
        @"min" : @(p.minValue),
        @"max" : @(p.maxValue),
        @"default" : @(p.defaultValue),
        @"unit" : @(p.unit),
        @"flags" : @(p.flags),
        @"flags_decoded" : flagNames(p.flags),
    } mutableCopy];
    if (p.flags & kAudioUnitParameterFlag_HasClump) d[@"clump"] = @(p.clumpID);
    if (p.unit == kAudioUnitParameterUnit_CustomUnit && p.unitName)
        d[@"unit_name"] = (__bridge NSString*)p.unitName;
    Float32 cur = 0;
    if (AudioUnitGetParameter(u, id, kAudioUnitScope_Global, 0, &cur) == noErr) d[@"value"] = @(cur);
    CFArrayRef strings = nullptr;
    size = sizeof(strings);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id,
                             &strings, &size) == noErr && strings)
        d[@"value_strings"] = CFBridgingRelease(strings);
    // Display strings at 11 evenly spaced values across the range.
    NSMutableArray* samples = [NSMutableArray array];
    for (int i = 0; i <= 10; ++i) {
        float v = p.minValue + (p.maxValue - p.minValue) * i / 10.0f;
        NSString* s = paramString(u, id, v);
        [samples addObject:@{@"value" : @(v), @"string" : s ?: [NSNull null]}];
    }
    d[@"samples"] = samples;
    return d;
}

static void writeJson(id obj, NSString* path)
{
    NSError* err = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:obj
                                                   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                     error:&err];
    if (!data) { fprintf(stderr, "error: JSON: %s\n", err.description.UTF8String); exit(1); }
    if (path) [data writeToFile:path atomically:YES];
    else fwrite(data.bytes, 1, data.length, stdout), fputc('\n', stdout);
}

static NSDictionary* hostInfo(const Host& h)
{
    return @{
        @"name" : h.name,
        @"version" : @(h.version),
        @"version_string" : [NSString stringWithFormat:@"%u.%u.%u", h.version >> 16,
                                                       (h.version >> 8) & 0xff, h.version & 0xff],
        @"type" : @(fourccString(h.desc.componentType).c_str()),
        @"subtype" : @(fourccString(h.desc.componentSubType).c_str()),
        @"manufacturer" : @(fourccString(h.desc.componentManufacturer).c_str()),
    };
}

// ---------------------------------------------------------------------------------------------
// Rendering

struct Stimulus {
    const float* data = nullptr;
    size_t length = 0;
    double gain = 1.0;
    size_t offset = 0; // position where the stimulus starts (in rendered samples)
    size_t pos = 0;    // current rendered sample
};

static OSStatus inputCallback(void* ctx, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                              UInt32 n, AudioBufferList* io)
{
    auto* s = static_cast<Stimulus*>(ctx);
    for (UInt32 i = 0; i < n; ++i) {
        size_t k = s->pos + i;
        float x = 0.0f;
        if (s->data && k >= s->offset && k - s->offset < s->length)
            x = static_cast<float>(s->data[k - s->offset] * s->gain);
        for (UInt32 b = 0; b < io->mNumberBuffers; ++b) static_cast<float*>(io->mBuffers[b].mData)[i] = x;
    }
    return noErr;
}

struct Renderer {
    AudioUnit u;
    UInt32 channels;
    UInt32 block;
    Stimulus stim;
    double sampleTime = 0;
    std::vector<std::vector<float>> buf;
    // Optional: output parameters (meters, e.g. a gain-reduction meter) read once per block while the
    // output is recorded; one row per block, one column per id.
    std::vector<AudioUnitParameterID> watch;
    std::vector<std::vector<float>> watched;
    // Run the main run loop for one block's duration per block, i.e. render in real time with the message thread
    // running, as a host plays: plugins that apply parameter changes on a main-thread timer see a mid-render change
    // only then (offline rendering is far faster than real time).
    bool pumpRunLoop = false;
    double fs = 48000.0;

    // Render n samples; if out != nullptr append interleaved output.
    void render(size_t n, std::vector<float>* out)
    {
        size_t done = 0;
        while (done < n) {
            UInt32 m = (UInt32)std::min<size_t>(block, n - done);
            struct {
                UInt32 count;
                AudioBuffer b[2];
            } abl;
            abl.count = channels;
            for (UInt32 c = 0; c < channels; ++c) abl.b[c] = {1, m * 4, buf[c].data()};
            AudioTimeStamp t = {};
            t.mSampleTime = sampleTime;
            t.mFlags = kAudioTimeStampSampleTimeValid;
            AudioUnitRenderActionFlags flags = 0;
            check(AudioUnitRender(u, &flags, &t, 0, m, (AudioBufferList*)&abl), "render");
            if (out)
                for (UInt32 i = 0; i < m; ++i)
                    for (UInt32 c = 0; c < channels; ++c)
                        out->push_back(static_cast<float*>(abl.b[c].mData)[i]);
            if (out && !watch.empty()) {
                std::vector<float> row;
                for (AudioUnitParameterID id : watch) {
                    Float32 v = NAN;
                    AudioUnitGetParameter(u, id, kAudioUnitScope_Global, 0, &v);
                    row.push_back(v);
                }
                watched.push_back(std::move(row));
            }
            if (pumpRunLoop) CFRunLoopRunInMode(kCFRunLoopDefaultMode, m / fs, false);
            sampleTime += m;
            stim.pos += m;
            done += m;
        }
    }
};

// A host instance prepared for rendering: format, input callback, initialized.
struct Session {
    Host h;
    Renderer r;
    NSData* stimData = nil;
};

static void openSession(Session& s, NSString* spec, double fs, UInt32 block)
{
    s.h = openHost(spec);
    AudioUnit u = s.h.unit;
    UInt32 channels = 2;
    AudioStreamBasicDescription fmt = {
        fs, kAudioFormatLinearPCM,
        (AudioFormatFlags)kAudioFormatFlagsNativeFloatPacked | (AudioFormatFlags)kAudioFormatFlagIsNonInterleaved,
        4, 1, 4, channels, 32, 0};
    bool fmtOk = true;
    for (auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output})
        fmtOk = fmtOk && AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, scope, 0, &fmt,
                                              sizeof(fmt)) == noErr;
    if (!fmtOk) {
        channels = 1;
        fmt.mChannelsPerFrame = 1;
        for (auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output})
            check(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, scope, 0, &fmt, sizeof(fmt)),
                  "stream format");
    }
    check(AudioUnitSetProperty(u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                               &block, sizeof(block)),
          "max frames");
    s.r.u = u;
    s.r.channels = channels;
    s.r.fs = fs;
    s.r.block = block;
    s.r.buf.assign(channels, std::vector<float>(block));
    AURenderCallbackStruct cb = {inputCallback, &s.r.stim};
    check(AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                               sizeof(cb)),
          "render callback");
    check(AudioUnitInitialize(u), "initialize");
    UInt32 bypass = 0;
    AudioUnitSetProperty(u, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &bypass,
                         sizeof(bypass));
}

static bool matches(float got, float want)
{
    return std::fabs(got - want) <= 1e-4f * std::max(1.0f, std::fabs(want));
}

// Set parameters, then render silent blocks and spin the main run loop until every
// parameter reads back as requested (some hosts' plugins apply changes asynchronously).
// Returns the number of silent blocks it took, or -1 on timeout.
static int applyParams(Session& s, NSDictionary* params, NSMutableDictionary* readback, int maxBlocks = 400)
{
    AudioUnit u = s.h.unit;
    const double savedGain = s.r.stim.gain;
    s.r.stim.gain = 0.0;
    int blocks = -1;
    for (int attempt = 0; attempt <= maxBlocks; ++attempt) {
        if (attempt % 50 == 0) // (re)send: covers plugins that drop sets while busy
            for (NSString* key in params)
                check(AudioUnitSetParameter(u, (AudioUnitParameterID)key.longLongValue,
                                            kAudioUnitScope_Global, 0, [params[key] floatValue], 0),
                      "set parameter");
        bool all = true;
        for (NSString* key in params) {
            Float32 got = 0;
            check(AudioUnitGetParameter(u, (AudioUnitParameterID)key.longLongValue, kAudioUnitScope_Global,
                                        0, &got),
                  "get parameter");
            all = all && matches(got, [params[key] floatValue]);
        }
        if (all) { blocks = attempt; break; }
        s.r.render(s.r.block, nullptr);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
    }
    // Like a real host, keep the main run loop going briefly after every change, so plugins that
    // update derived state on their message thread (e.g. our Auto Gain) see the new settings.
    for (int i = 0; i < 10; ++i) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
    s.r.stim.gain = savedGain;
    if (readback)
        for (NSString* key in params) {
            AudioUnitParameterID id = (AudioUnitParameterID)key.longLongValue;
            Float32 got = 0;
            AudioUnitGetParameter(u, id, kAudioUnitScope_Global, 0, &got);
            readback[key] = @{@"requested" : params[key], @"got" : @(got),
                              @"string" : paramString(u, id, got) ?: [NSNull null]};
            if (!matches(got, [params[key] floatValue]))
                fprintf(stderr, "warning: param %u requested %g, read back %g\n", id,
                        [params[key] floatValue], got);
        }
    return blocks;
}

static bool checkParams(AudioUnit u, NSDictionary* params, NSMutableDictionary* readback)
{
    bool ok = true;
    for (NSString* key in params) {
        Float32 got = 0;
        check(AudioUnitGetParameter(u, (AudioUnitParameterID)key.longLongValue, kAudioUnitScope_Global, 0,
                                    &got),
              "get parameter");
        readback[key] = @(got);
        ok = ok && matches(got, [params[key] floatValue]);
    }
    return ok;
}

static int cmdRender(int argc, char** argv)
{
    NSString* spec = argValue(argc, argv, "--plugin");
    NSString* jobPath = argValue(argc, argv, "--job");
    NSString* outDir = argValue(argc, argv, "--out");
    if (!spec || !jobPath || !outDir) return 2;
    NSDictionary* job = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:jobPath]
                                                        options:0 error:nil];
    if (!job) { fprintf(stderr, "error: cannot read job %s\n", jobPath.UTF8String); return 2; }
    [[NSFileManager defaultManager] createDirectoryAtPath:outDir withIntermediateDirectories:YES
                                               attributes:nil error:nil];

    const double fs = [job[@"sample_rate"] doubleValue] ?: 48000.0;
    const UInt32 block = [job[@"block_size"] unsignedIntValue] ?: 512;
    const double settle = job[@"settle_s"] ? [job[@"settle_s"] doubleValue] : 0.5;
    const double preroll = job[@"preroll_s"] ? [job[@"preroll_s"] doubleValue] : 0.5;
    const double tail = job[@"tail_s"] ? [job[@"tail_s"] doubleValue] : 2.0;

    NSData* stimData = [NSData dataWithContentsOfFile:job[@"stimulus"]];
    if (!stimData) { fprintf(stderr, "error: cannot read stimulus\n"); return 2; }

    Session s;
    openSession(s, spec, fs, block);
    AudioUnit u = s.h.unit;
    Renderer& r = s.r;
    r.stim.data = static_cast<const float*>(stimData.bytes);
    r.stim.length = stimData.length / sizeof(float);

    NSDictionary* base = job[@"base_params"] ?: @{};
    NSArray* runs = job[@"runs"];
    const size_t nSettle = (size_t)std::llround(settle * fs);
    const size_t nPre = (size_t)std::llround(preroll * fs);
    const size_t nTail = (size_t)std::llround(tail * fs);
    int failures = 0;

    for (NSDictionary* run in runs) {
        NSString* runName = run[@"name"];
        NSMutableDictionary* params = [base mutableCopy];
        [params addEntriesFromDictionary:run[@"params"] ?: @{}];
        NSMutableDictionary* readback = [NSMutableDictionary dictionary];

        // Settle: apply params (wait for read-back), render silence so internal
        // smoothing finishes, reset (clears filter state), then pre-roll silence.
        r.stim.gain = 0.0;
        const int blocks = applyParams(s, params, nil);
        r.render(nSettle, nullptr);
        check(AudioUnitReset(u, kAudioUnitScope_Global, 0), "reset");
        bool ok = applyParams(s, params, readback) >= 0 && blocks >= 0;
        r.render(nPre, nullptr);

        r.stim.gain = run[@"gain"] ? [run[@"gain"] doubleValue] : 1.0;
        r.stim.offset = r.stim.pos;
        std::vector<float> out;
        out.reserve((r.stim.length + nTail) * r.channels);
        // Optional mid-render change {"at_s": t, "params": {...}}: set at that time, block-aligned, without
        // waiting for read-back (measures the plugin's own parameter smoothing / settle time).
        NSDictionary* change = run[@"change"];
        int changeAppliedBlock = -1;
        r.watch.clear();
        r.watched.clear();
        for (NSNumber* id in (job[@"watch"] ?: @[])) r.watch.push_back((AudioUnitParameterID)id.unsignedIntValue);
        const size_t total = r.stim.length + nTail;
        if (change) {
            size_t at = (size_t)std::llround([change[@"at_s"] doubleValue] * fs);
            at = std::min(total, at / block * block);
            r.render(at, &out);
            NSDictionary* cp = change[@"params"];
            auto send = [&] {
                for (NSString* key in cp)
                    check(AudioUnitSetParameter(u, (AudioUnitParameterID)key.longLongValue, kAudioUnitScope_Global, 0,
                                                [cp[key] floatValue], 0),
                          "set parameter (change)");
            };
            send();
            [params addEntriesFromDictionary:cp];
            // Some plugins apply a set asynchronously on the main thread and may drop it while busy: keep the
            // run loop going and re-send every 4 blocks until it reads back (at most 1 s), as a host would.
            r.pumpRunLoop = true;
            size_t done = at;
            int appliedBlock = -1;
            NSMutableDictionary* tmp = [NSMutableDictionary dictionary];
            for (int b = 0; done < total; ++b) {
                if (appliedBlock < 0 && checkParams(u, cp, tmp)) appliedBlock = b;
                if (appliedBlock < 0 && b > 0 && b % 4 == 0 && (double)b * block < fs) send();
                const size_t m = std::min<size_t>(block, total - done);
                r.render(m, &out);
                done += m;
            }
            r.pumpRunLoop = false;
            changeAppliedBlock = appliedBlock;
        } else {
            r.render(total, &out);
        }
        r.stim.gain = 0.0;

        // The setting must not have drifted during the render.
        NSMutableDictionary* after = [NSMutableDictionary dictionary];
        ok = checkParams(u, params, after) && ok;

        Float64 latency = 0;
        UInt32 sz = sizeof(latency);
        AudioUnitGetProperty(u, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &sz);
        Float64 tailTime = 0;
        sz = sizeof(tailTime);
        AudioUnitGetProperty(u, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &tailTime, &sz);

        NSString* audioPath = [outDir stringByAppendingPathComponent:[runName stringByAppendingString:@".f32"]];
        [[NSData dataWithBytes:out.data() length:out.size() * sizeof(float)] writeToFile:audioPath
                                                                              atomically:YES];
        NSMutableDictionary* meta = [@{
            @"name" : runName,
            @"plugin" : hostInfo(s.h),
            @"sample_rate" : @(fs),
            @"block_size" : @(block),
            @"channels" : @(r.channels),
            @"frames" : @(out.size() / r.channels),
            @"stimulus_frames" : @(r.stim.length),
            @"gain" : run[@"gain"] ?: @1.0,
            @"settle_s" : @(settle),
            @"preroll_s" : @(preroll),
            @"param_apply_blocks" : @(blocks),
            @"latency_reported_s" : @(latency),
            @"tail_time_s" : @(tailTime),
            @"readback" : readback,
            @"readback_after" : after,
            @"params_ok" : @(ok),
        } mutableCopy];
        if (run[@"meta"]) meta[@"meta"] = run[@"meta"];
        if (change) {
            meta[@"change"] = change;
            meta[@"change_applied_block"] = @(changeAppliedBlock);   // blocks after the change until it read back
        }
        if (!r.watch.empty()) {   // {"<id>": [value per block]}, blocks of block_size from the recording's start
            NSMutableDictionary* w = [NSMutableDictionary dictionary];
            for (size_t j = 0; j < r.watch.size(); ++j) {
                NSMutableArray* col = [NSMutableArray arrayWithCapacity:r.watched.size()];
                for (const auto& row : r.watched) [col addObject:@(row[j])];
                w[[NSString stringWithFormat:@"%u", r.watch[j]]] = col;
            }
            meta[@"watch"] = w;
        }
        writeJson(meta, [outDir stringByAppendingPathComponent:[runName stringByAppendingString:@".json"]]);
        fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", runName.UTF8String);
        if (!ok) ++failures;
    }
    AudioUnitUninitialize(u);
    AudioComponentInstanceDispose(u);
    return failures ? 4 : 0;
}

// ---------------------------------------------------------------------------------------------

static int cmdList(int argc, char** argv)
{
    NSString* filter = argc > 2 ? @(argv[2]) : nil;
    for (OSType t : {kAudioUnitType_Effect, kAudioUnitType_MusicEffect}) {
        AudioComponentDescription d = {};
        d.componentType = t;
        AudioComponent c = nullptr;
        while ((c = AudioComponentFindNext(c, &d))) {
            NSString* n = componentName(c);
            if (filter && ![n localizedCaseInsensitiveContainsString:filter]) continue;
            AudioComponentDescription cd;
            AudioComponentGetDescription(c, &cd);
            UInt32 v = 0;
            AudioComponentGetVersion(c, &v);
            printf("%s:%s:%s  %s  (version %u.%u.%u)\n", fourccString(cd.componentType).c_str(),
                   fourccString(cd.componentSubType).c_str(), fourccString(cd.componentManufacturer).c_str(),
                   n.UTF8String, v >> 16, (v >> 8) & 0xff, v & 0xff);
        }
    }
    return 0;
}

static int cmdParams(int argc, char** argv)
{
    NSString* spec = argValue(argc, argv, "--plugin");
    if (!spec) return 2;
    Host h = openHost(spec);
    check(AudioUnitInitialize(h.unit), "initialize");
    NSMutableArray* list = [NSMutableArray array];
    for (auto id : paramIds(h.unit)) [list addObject:paramInfo(h.unit, id)];
    writeJson(@{@"plugin" : hostInfo(h), @"unit" : unitInfo(h.unit), @"parameters" : list}, argValue(argc, argv, "--json"));
    if (argValue(argc, argv, "--json"))
        for (NSDictionary* p in list)
            printf("%6u  %-28s  %10g .. %-10g default %-8g unit %u\n", [p[@"id"] unsignedIntValue],
                   [p[@"name"] UTF8String], [p[@"min"] doubleValue], [p[@"max"] doubleValue],
                   [p[@"default"] doubleValue], [p[@"unit"] unsignedIntValue]);
    return 0;
}

// Set one parameter to each value, read it back, and record the display string.
static int cmdProbe(int argc, char** argv)
{
    NSString* spec = argValue(argc, argv, "--plugin");
    NSString* pid = argValue(argc, argv, "--param");
    NSString* vals = argValue(argc, argv, "--values");
    if (!spec || !pid || !vals) return 2;
    Session s;
    openSession(s, spec, 48000.0, 512);
    AudioUnitParameterID id = (AudioUnitParameterID)pid.longLongValue;
    NSMutableArray* res = [NSMutableArray array];
    for (NSString* str in [vals componentsSeparatedByString:@","]) {
        NSDictionary* p = @{pid : @(str.floatValue)};
        NSMutableDictionary* rb = [NSMutableDictionary dictionary];
        const int blocks = applyParams(s, p, rb);
        NSMutableDictionary* e = [rb[pid] mutableCopy];
        e[@"blocks"] = @(blocks);
        [res addObject:e];
    }
    writeJson(@{@"plugin" : hostInfo(s.h), @"param" : @(id), @"probe" : res}, argValue(argc, argv, "--json"));
    return 0;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: lkit_measure list|params|probe|render ...\n");
            return 2;
        }
        std::string cmd = argv[1];
        int rc = 2;
        if (cmd == "list") rc = cmdList(argc, argv);
        else if (cmd == "params" || cmd == "--list-params") rc = cmdParams(argc, argv);
        else if (cmd == "probe") rc = cmdProbe(argc, argv);
        else if (cmd == "render") rc = cmdRender(argc, argv);
        if (rc == 2) fprintf(stderr, "error: bad arguments for '%s'\n", cmd.c_str());
        // Exit without running static destructors: some plugins (e.g. the Pre 73 reference) abort in
        // their own teardown at process exit ("mutex lock failed"). All output is written by now.
        fflush(stdout);
        fflush(stderr);
        _exit(rc);
    }
}
