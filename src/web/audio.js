// The AudioContext and the worklet that plays the game's audio ring.
import { AUD_CONSUMER, AUD_HEADER_WORDS, AUD_SAMPLE_RATE } from './layout.js';

// Call synchronously inside a user gesture: some browsers only let a context
// start there.
export function createAudioContext() {
	let context;
	try {
		// At the ring's own rate the browser does the (better) resampling.
		context = new AudioContext({ sampleRate: AUD_SAMPLE_RATE, latencyHint: 'interactive' });
	} catch {
		context = new AudioContext({ latencyHint: 'interactive' });
	}
	context.resume().catch((error) => console.warn('Audio could not start', error));
	return context;
}

// Connects the ring at address to the context; afterwards the game feeds it
// (AUD_CONSUMER), so call this before main() starts.
export async function startAudio(context, buffer, address, volume) {
	await context.audioWorklet.addModule(new URL('./audio-worklet.js', import.meta.url));
	const node = new AudioWorkletNode(context, 'nfs2se-ring', {
		numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: { buffer, address },
	});
	const gain = new GainNode(context, { gain: volume });
	node.connect(gain).connect(context.destination);
	Atomics.store(new Int32Array(buffer, address, AUD_HEADER_WORDS), AUD_CONSUMER, 1);
	return {
		setVolume(value) {
			gain.gain.value = value;
		},
		// Never rejects; a closed context only logs a warning.
		setPaused(paused) {
			return (paused ? context.suspend() : context.resume()).catch((error) => console.warn('Audio could not pause or resume', error));
		},
	};
}
