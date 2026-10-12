/* Model + selected-engine contracts, not global catalog or loaded-worker state. */
(function (root) {
  function accepts(contract, operation, inputs, output) {
    const required = contract.required_inputs || [], optional = contract.optional_inputs || [];
    return contract.operation === operation && required.every(x => inputs.includes(x)) &&
      inputs.every(x => required.includes(x) || optional.includes(x)) &&
      (contract.outputs || []).includes(output);
  }
  function supports(row, operation, inputs, output, tools = false, streaming = false) {
    return !!row && (row.supported_interactions || []).some(x => accepts(x, operation, inputs, output)) &&
      (row.endpoint_contracts || []).some(x => accepts(x, operation, inputs, output) &&
        (!tools || (x.supports_tools && (row.abilities || []).includes('tool_calling'))) &&
        (!streaming || x.streaming));
  }
  function controls(selected, streaming = false) {
    const chat = supports(selected.llm, 'chat.generate', ['text'], 'text', false, streaming);
    const tools = chat && supports(selected.llm, 'chat.generate', ['text'], 'text', true);
    const image = tools && supports(selected.vlm, 'chat.generate', ['text', 'image'], 'text');
    return {
      chat, image, document: image,
      video: tools && supports(selected.vlm, 'chat.generate', ['text', 'video'], 'text'),
      voice: chat && supports(selected.asr, 'audio.transcribe', ['audio'], 'text'),
      speech: chat && supports(selected.tts, 'audio.synthesize_speech', ['text'], 'audio', false, streaming),
      reference: supports(selected.tts, 'audio.synthesize_speech', ['text', 'audio'], 'audio', false, streaming),
    };
  }
  const api = { supports, controls };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.MicaCapabilities = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
