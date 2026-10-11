// Plays the shared audio ring filled by src/platform/sound.c: int16 stereo
// frames at the ring's rate, linearly resampled to the context rate.
import {
	AUD_ACTIVE, AUD_CAPACITY_FRAMES, AUD_HEADER_WORDS, AUD_RATE, AUD_READ, AUD_WRITE,
} from './layout.js';

class RingPlayer extends AudioWorkletProcessor {
	constructor(options) {
		super();
		const { buffer, address } = options.processorOptions;
		this.header = new Int32Array(buffer, address, AUD_HEADER_WORDS);
		this.frames = new Int16Array(buffer, address + AUD_HEADER_WORDS * 4, AUD_CAPACITY_FRAMES * 2);
		this.step = Atomics.load(this.header, AUD_RATE) / sampleRate;
		this.position = 0; // source frames past AUD_READ
	}

	process(inputs, outputs) {
		const [left, right] = outputs[0];
		const header = this.header, frames = this.frames, mask = AUD_CAPACITY_FRAMES - 1;
		const write = Atomics.load(header, AUD_WRITE), read = Atomics.load(header, AUD_READ);
		let i = 0;
		if (Atomics.load(header, AUD_ACTIVE)) {
			const available = (write - read) | 0;
			let position = this.position;
			for (; i < left.length; ++i) {
				const index = Math.floor(position);
				if (index + 1 >= available)
					break; // underrun: silence until the producer catches up
				const a = ((read + index) & mask) * 2, b = ((read + index + 1) & mask) * 2, t = position - index;
				left[i] = (frames[a] + (frames[b] - frames[a]) * t) / 32768;
				right[i] = (frames[a + 1] + (frames[b + 1] - frames[a + 1]) * t) / 32768;
				position += this.step;
			}
			const consumed = Math.min(Math.floor(position), available);
			this.position = position - consumed;
			if (consumed) {
				Atomics.store(header, AUD_READ, (read + consumed) | 0);
				Atomics.notify(header, AUD_READ);
			}
		} else if (read !== write) {
			// Stopped: drop what is still queued.
			Atomics.store(header, AUD_READ, write);
			Atomics.notify(header, AUD_READ);
			this.position = 0;
		}
		left.fill(0, i);
		right.fill(0, i);
		return true;
	}
}

registerProcessor('nfs2se-ring', RingPlayer);
