#!/usr/bin/env python3
"""Stop the music player's audio graph coming up connected to nothing.

Why this exists
---------------
Build 565 shipped a player that aborted, and then -- once the exception was
caught -- a play button that did nothing at all. The cause was one line in
app/AudioPlayerEngine.m's connectPlayerWithFormat:, which decided the graph was
already built by comparing formats:

    if ([[self.playerNode outputFormatForBus:0] isEqual:format] && ...)
        return YES;

An AVAudioPlayerNode that has been attached to an engine and joined to nothing
still answers -outputFormatForBus: with 44100 Hz stereo -- the standard format
of ordinary music, and the very format -init connects with, since it takes the
mixer's default. So the test said "already connected" about a graph with no
connections in it, the player node was never joined to the equalizer or the
mixer, and -[AVAudioPlayerNode play] threw "player started when in a
disconnected state" on every 44.1 kHz track. Anything at another sample rate
connected normally, which is why it looked like a problem with one folder.

A reported format is not evidence of a connection. The only thing that answers
that question is -outputConnectionPointsForNode:outputBus:.

    tools/check-audio-graph.py [path/to/AudioPlayerEngine.m]

Two halves, both of which must pass:

1. The source. connectPlayerWithFormat: has to decide "already wired" through
   playerIsWiredForFormat:, that predicate has to ask the engine for the
   connections of BOTH nodes, and the connect has to check its own work
   afterwards rather than returning YES on the strength of having tried. A
   format comparison inside connectPlayerWithFormat: itself is the exact shape
   of the regression and fails here.

2. The platform. A short AVFoundation program checks that the hazard is still
   real on this macOS and that the predicate the app uses is right about it: an
   attached, unconnected node reports a usable format and zero connections,
   play throws in that state, and after connecting it reports connections and
   plays. If Apple ever changes any of that, this says so rather than leaving
   the app resting on an assumption that has quietly moved.

The positive control is free and needs no edit, because the broken version is
in the history:

    git show 26287d6ff:app/AudioPlayerEngine.m > /tmp/bad.m
    tools/check-audio-graph.py /tmp/bad.m      # must exit 1

Exits 1 on any failure, 0 when the graph cannot come up silently unconnected.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = os.path.join(HERE, 'app', 'AudioPlayerEngine.m')

CONNECT = 'connectPlayerWithFormat:(AVAudioFormat *)format'
PREDICATE = 'playerIsWiredForFormat:(AVAudioFormat *)format'
CONNECTIONS = 'outputConnectionPointsForNode:'


def method_body(source, signature):
    """The text of one Objective-C method, by brace matching from its start."""
    start = source.find(signature)
    if start < 0:
        return None
    i = source.find('{', start)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(source)):
        if source[j] == '{':
            depth += 1
        elif source[j] == '}':
            depth -= 1
            if depth == 0:
                return source[i:j + 1]
    return None


def check_source(path):
    bad = False
    with open(path, encoding='utf-8') as handle:
        source = handle.read()

    predicate = method_body(source, PREDICATE)
    if predicate is None:
        print(f'\n{path}: no -{PREDICATE.split("(")[0]} at all. The decision '
              f'about whether the graph is already built has to live in one '
              f'place that asks the engine; see the header of this script for '
              f'what happens when it is inlined as a format comparison.')
        return True

    asks = predicate.count(CONNECTIONS)
    if asks < 2:
        bad = True
        print(f'\n{path}: playerIsWiredForFormat: asks {CONNECTIONS} {asks} '
              f'time(s); it needs both the player node and the equalizer node. '
              f'A node reports a format whether or not it is joined to '
              f'anything, so the format comparison alone proves nothing.')

    connect = method_body(source, CONNECT)
    if connect is None:
        print(f'\n{path}: no -{CONNECT.split("(")[0]} found.')
        return True

    uses = connect.count('playerIsWiredForFormat:')
    if uses < 2:
        bad = True
        print(f'\n{path}: connectPlayerWithFormat: consults '
              f'playerIsWiredForFormat: {uses} time(s). It needs it twice: '
              f'once to decide there is nothing to do, and once afterwards to '
              f'confirm the connect actually built the graph. Returning YES '
              f'because the calls did not throw is what shipped in 565.')

    if 'outputFormatForBus' in connect:
        bad = True
        print(f'\n{path}: connectPlayerWithFormat: compares formats itself. '
              f'That is the regression: an unconnected node reports the format '
              f'you are about to connect with, so the comparison says "already '
              f'done" about an empty graph. Ask playerIsWiredForFormat:.')

    return bad


PROBE = r'''
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

static BOOL wired(AVAudioEngine *e, AVAudioNode *p, AVAudioNode *q, AVAudioFormat *f) {
    return [e outputConnectionPointsForNode:p outputBus:0].count > 0 &&
           [e outputConnectionPointsForNode:q outputBus:0].count > 0 &&
           [[p outputFormatForBus:0] isEqual:f] && [[q outputFormatForBus:0] isEqual:f];
}

int main(void) {
  @autoreleasepool {
    AVAudioEngine *e = [AVAudioEngine new];
    AVAudioPlayerNode *p = [AVAudioPlayerNode new];
    AVAudioUnitEQ *q = [[AVAudioUnitEQ alloc] initWithNumberOfBands:5];
    [e attachNode:p];
    [e attachNode:q];
    AVAudioFormat *fmt = [e.mainMixerNode outputFormatForBus:0];

    printf("unconnected_reports_a_format %d\n",
           (int)([p outputFormatForBus:0].sampleRate > 0));
    printf("unconnected_format_equals_the_one_we_would_connect %d\n",
           (int)[[p outputFormatForBus:0] isEqual:fmt]);
    printf("unconnected_connection_count %lu\n",
           (unsigned long)[e outputConnectionPointsForNode:p outputBus:0].count);
    printf("predicate_says_wired_while_unconnected %d\n", (int)wired(e, p, q, fmt));

    if (e.isRunning) [e stop];
    [e connect:p to:q format:fmt];
    [e connect:q to:e.mainMixerNode format:fmt];
    printf("predicate_says_wired_after_connecting %d\n", (int)wired(e, p, q, fmt));

    NSError *err = nil;
    if (![e startAndReturnError:&err]) {
        printf("engine_started 0\n");
        return 0;
    }
    printf("engine_started 1\n");
    int ok = 0;
    @try { [p play]; ok = p.isPlaying ? 1 : 0; }
    @catch (NSException *ex) { ok = 0; }
    printf("plays_once_connected %d\n", ok);
  }
  return 0;
}
'''


def check_platform():
    """The AVFoundation behaviours the app's predicate is built on."""
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'probe.m')
        exe = os.path.join(tmp, 'probe')
        with open(src, 'w', encoding='utf-8') as handle:
            handle.write(PROBE)
        build = subprocess.run(
            ['clang', '-fobjc-arc', '-Wno-deprecated-declarations',
             '-framework', 'Foundation', '-framework', 'AVFoundation',
             '-o', exe, src],
            capture_output=True, text=True)
        if build.returncode != 0:
            print('\nCould not build the AVFoundation probe, so the platform '
                  'half of this check did not run:\n' + build.stderr.strip())
            return True
        run = subprocess.run([exe], capture_output=True, text=True)
        if run.returncode != 0:
            print('\nThe AVFoundation probe did not run:\n' + run.stderr.strip())
            return True

    facts = {}
    for line in run.stdout.split('\n'):
        parts = line.split()
        if len(parts) == 2:
            facts[parts[0]] = int(parts[1])

    bad = False
    if facts.get('engine_started') != 1:
        print('\nNo audio engine on this machine (no output device?), so only '
              'the source half of this check ran.')
        return False

    # What the app relies on. Each line says what breaks if it stops holding.
    expectations = [
        ('unconnected_connection_count', 0,
         'an unconnected node must report no connections -- that is the only '
         'thing the predicate can trust'),
        ('predicate_says_wired_while_unconnected', 0,
         'the predicate must not call an unconnected graph wired'),
        ('predicate_says_wired_after_connecting', 1,
         'the predicate must recognise a graph it just connected, or every '
         'track would reconnect and stop a running engine'),
        ('plays_once_connected', 1,
         'a connected player node must start'),
    ]
    for key, want, why in expectations:
        got = facts.get(key)
        if got != want:
            bad = True
            print(f'\nAVFoundation changed under us: {key} is {got}, expected '
                  f'{want} -- {why}.')

    if facts.get('unconnected_reports_a_format') == 1 and \
            facts.get('unconnected_format_equals_the_one_we_would_connect') == 1:
        hazard = ('present: an unconnected node still reports exactly the '
                  'format -init would connect with')
    else:
        hazard = ('gone on this OS, but the source check stays -- it costs '
                  'nothing and the behaviour has changed before')
    print(f'AVFoundation: the format-is-not-a-connection hazard is {hazard}.')
    return bad


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SOURCE
    if not os.path.exists(path):
        print(f'{path}: no such file')
        return 1
    bad = check_source(path)
    bad = check_platform() or bad
    if bad:
        print('\nThe music player can come up with its graph connected to '
              'nothing. See the header of this script.')
        return 1
    print(f'{os.path.relpath(path, HERE)}: a connect reports success only '
          f'after asking the engine for both nodes\' connections, so the audio '
          f'graph cannot come up silently unconnected.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
