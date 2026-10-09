#!/usr/bin/env python3
"""Dependency-free tests for playground API-key handling."""

from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "mica_playground", ROOT / "apps/mica_playground.py"
)
PLAYGROUND = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PLAYGROUND)


class PlaygroundAuthTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node is optional; required only for frontend tests")
    def test_session_ids_work_on_http_lan_without_random_uuid(self) -> None:
        page = PLAYGROUND.CHAT_HTML
        helper = re.search(r"function newSessionId\(\)\{.*?\n\}", page, re.S)
        self.assertIsNotNone(helper)
        script = r"""
const assert = require('node:assert/strict');
const vm = require('node:vm');
const {webcrypto} = require('node:crypto');
const helper = HELPER;
const lan = vm.createContext({crypto: {getRandomValues: array => webcrypto.getRandomValues(array)}});
vm.runInContext(helper, lan);
const ids = new Set();
for (let i = 0; i < 100; i++) {
  const id = vm.runInContext('newSessionId()', lan);
  assert.match(id, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  ids.add(id);
}
assert.equal(ids.size, 100);
const secure = vm.createContext({crypto: {randomUUID: () => 'native-uuid'}});
vm.runInContext(helper, secure);
assert.equal(vm.runInContext('newSessionId()', secure), 'native-uuid');
""".replace("HELPER", json.dumps(helper.group(0)))
        subprocess.run(["node", "-e", script], check=True)
        self.assertIn("localStorage.micaSessionId||newSessionId()", page)
        self.assertIn("sessionId=newSessionId();localStorage", page)
        self.assertEqual(page.count("crypto.randomUUID()"), 1)

    @unittest.skipUnless(shutil.which("node"), "Node is optional; required only for frontend tests")
    def test_frontend_capability_matrix(self) -> None:
        subprocess.run(["node", str(ROOT / "tests/test_playground_capabilities.js")], check=True)

    def test_chat_refreshes_active_models_and_validates_before_sending(self) -> None:
        page = PLAYGROUND.CHAT_HTML
        self.assertIn('src="/mica-capabilities.js"', page)
        self.assertIn("await refreshModels();validateInput()", page)
        self.assertIn("setInterval(()=>", page)
        self.assertIn("f.append('speech_reply',String(c.speech))", page)
        self.assertIn("$('files').disabled=busy||!canAttach", page)

    @staticmethod
    def handler(key: str = "", fallback: str = ""):
        handler = object.__new__(PLAYGROUND.Handler)
        handler.headers = {"X-Mica-API-Key": key} if key else {}
        handler.server = SimpleNamespace(api_key=fallback)
        return handler

    def test_missing_key_returns_explicit_401(self) -> None:
        handler = self.handler()
        sent = []
        handler._send = lambda status, content_type, body: sent.append(
            (status, content_type, body)
        )
        self.assertIsNone(handler._authorization_headers())
        self.assertEqual(sent[0][0], 401)
        self.assertIn(b'"code": "api_key_required"', sent[0][2])

    def test_tab_key_is_forwarded_as_bearer_token(self) -> None:
        handler = self.handler("tab-token-0123456789", "fallback-token")
        headers = handler._authorization_headers("application/json")
        self.assertEqual(headers["Authorization"], "Bearer tab-token-0123456789")
        self.assertEqual(headers["Content-Type"], "application/json")

    def test_cli_key_precedes_config_and_key_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            key_file = Path(temporary) / "key"
            key_file.write_text("file-token\n")
            args = Namespace(api_key="cli-token", api_key_file=key_file)
            config = {"api_key": "config-token", "api_key_file": str(key_file)}
            self.assertEqual(PLAYGROUND.configured_key(args, config), "cli-token")

    def test_chat_exposes_local_appearance_and_prompt_controls(self) -> None:
        page = PLAYGROUND.CHAT_HTML
        for control in (
            'id="theme"',
            'id="logoUpload"',
            'id="logoReset"',
            'id="llmSystemPrompt"',
            'id="vlmSystemPrompt"',
            'id="resetPrompts"',
        ):
            self.assertIn(control, page)
        self.assertIn("f.append('llm_system_prompt'", page)
        self.assertIn("f.append('vlm_system_prompt'", page)
        self.assertIn("data-mica-logo", page)

    def test_markdown_renderer_escapes_html_and_rejects_unsafe_links(self) -> None:
        page = PLAYGROUND.CHAT_HTML
        self.assertIn("const escapeHtml=", page)
        self.assertIn("https?:\\/\\/|mailto:|\\/", page)
        self.assertIn("host.querySelector('img')", page)
        self.assertIn("host.querySelectorAll('a').length!==1", page)


if __name__ == "__main__":
    unittest.main()
