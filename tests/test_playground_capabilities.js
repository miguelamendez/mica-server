const assert = require('node:assert/strict');
const {controls, supports} = require('../apps/mica-capabilities.js');

function row(operation, required, optional, output, tools = false, streaming = true) {
  return {state: 'stopped', abilities: tools ? ['tool_calling'] : [],
    supported_interactions: [{operation, required_inputs: required, optional_inputs: optional, outputs: [output]}],
    endpoint_contracts: [{operation, required_inputs: required, optional_inputs: optional, outputs: [output], supports_tools: tools, streaming}]};
}
const llm = row('chat.generate', ['text'], [], 'text', true);
const vlm = row('chat.generate', ['text'], ['image', 'video'], 'text');
const asr = row('audio.transcribe', ['audio'], [], 'text');
const tts = row('audio.synthesize_speech', ['text'], ['audio'], 'audio');

assert.deepEqual(controls({llm, vlm}, true), {
  chat: true, image: true, document: true, video: true, voice: false, speech: false, reference: false,
});
assert.equal(controls({llm}).image, false);
assert.equal(controls({llm, asr}).voice, true);
assert.equal(controls({llm, asr}).speech, false); // ASR does not imply TTS.
assert.equal(controls({llm, tts}).voice, false); // TTS does not imply ASR.
assert.equal(controls({llm, vlm, asr, tts}, true).reference, true);
assert.equal(controls({llm: {...llm, abilities: []}, vlm}).image, false);
assert.equal(controls({llm, vlm: row('chat.generate', ['text'], ['image'], 'text')}).video, false);
assert.equal(controls({llm: row('chat.generate', ['text'], [], 'text', true, false)}, true).chat, false);
assert.equal(controls({llm: {...llm, endpoint_contracts: []}, vlm}).chat, false);
assert.equal(controls({llm, vlm: {...vlm, endpoint_contracts: [llm.endpoint_contracts[0]]}}).image, false);
assert.equal(supports(vlm, 'chat.generate', ['image'], 'text'), false); // Missing required text.
assert.equal(supports(vlm, 'chat.generate', ['text', 'audio'], 'text'), false);
assert.equal(controls({llm, tts: row('audio.synthesize_speech', ['text'], [], 'audio')}).reference, false);
assert.equal(controls({}).chat, false);
// Swapping away from a visual/voice workload must immediately remove controls.
assert.equal(controls({llm, vlm, asr, tts}).voice, true);
assert.equal(controls({llm}).voice, false);
console.log('Playground capability matrix: passed');
