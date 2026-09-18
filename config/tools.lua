-- Agent utilities are declarative. C++ owns path validation, limits, execution,
-- and the tool-call loop; the language model only chooses when and how to call.
mica.vlm_tool {
  enabled = true,
  name = "vlm_tool",
  model_id = "minicpm-v46-thinking",
  description = "Inspect session-scoped images, videos, or rendered document pages before answering. Accepts at most 8 images, 1 video, or 8 document pages in one call, with no more than 8 total visual items. Video processing samples at most 32 frames. Use only paths listed in the current attachment manifest.",
  max_images_per_call = 8,
  max_videos_per_call = 1,
  max_document_pages_per_call = 8,
  max_video_frames = 32,
  max_total_visual_items = 8,
  max_agent_steps = 4,
  max_upload_bytes = 52428800,
}
