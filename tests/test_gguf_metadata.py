"""Metadata inspection never requires loading the model weights."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("gguf_metadata", Path(__file__).parents[1] / "scripts/gguf_metadata.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def string(value):
    encoded = value.encode()
    return struct.pack("<Q", len(encoded)) + encoded

class MetadataTests(unittest.TestCase):
    def test_header_without_tensor_payload(self):
        data = b"GGUF" + struct.pack("<IQQ", 3, 667, 3)
        data += string("general.architecture") + struct.pack("<I", 8) + string("gemma4")
        data += string("gemma4.attention.head_count_kv") + struct.pack("<IIQIII", 9, 4, 3, 8, 8, 1)
        data += string("tokenizer.ggml.tokens") + struct.pack("<IIQ", 9, 8, 2) + string("hello") + string("world")
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "header.gguf"
            file.write_bytes(data)
            result = module.inspect(file)
        self.assertEqual(result["tensor_count"], 667)
        self.assertEqual(result["metadata"], {"general.architecture": "gemma4", "gemma4.attention.head_count_kv": [8, 8, 1]})

    def test_bad_magic(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "bad.gguf"
            file.write_bytes(b"nope")
            with self.assertRaises(ValueError):
                module.inspect(file)

if __name__ == "__main__":
    unittest.main()
