# mod_telegram

Telegram audio calls and messaging implementation for *FreeSWITCH*.

## Dependencies

FreeSWITCH, tdlib, tgcalls

## Install

### Install from binary

The binary package is prepeared for Debian 12 Bookworm. Also there is a .tar.gz archive with binary module.

### Install from sources

Add this git repo to *FreeSWITCH* source tree under *src/mod/endpoints/mod_telegram*.

Add module dependencies to *FreeSWITCH* source tree under *lib/*.

Add hints to this module in *FreeeSWITCH* build subsystem.

Build module as any other *FreeSWITCH* module or as a part of entire *FreeSWITCH* build.

## Capabilities

This module is capable of making inbound and outbound audio calls only.
Video calls support is rather complicated to implement because of some FreeSWITCH architetural asymetry for audio and video paths.

## Versioning

Here is release version 2.0 of module.

---

## Configuration

`autoload_configs/telegram.conf.xml`:

```xml
<configuration name="telegram.conf" description="Telegram Endpoint">
  <profiles>
    <profile name="account1">
      <param name="api-id" value="YOUR_API_ID"/>
      <param name="api-hash" value="YOUR_API_HASH"/>
      <param name="phone" value="+1234567890"/>
    </profile>
  </profiles>
</configuration>
```

---

## Voice Calls

**Originate an outbound call:**
```
originate telegram/<profile>/<telegram_user_id> &echo
```

**Inbound calls** arrive as standard FreeSWITCH inbound sessions on the `telegram` endpoint.

---

## FreeSWITCH API Commands

All commands are available via `fs_cli` or ESL as `api <command> <args>`.

### Messaging

| Command | Arguments | Description |
|---|---|---|
| `tg_send_text` | `<profile> <chat_id> [reply_to:<msg_id>] <text>` | Send a plain text message |

### Media — Outbound

| Command | Arguments | Description |
|---|---|---|
| `tg_send_photo` | `<profile> <chat_id> <file_path> [caption]` | Send a photo |
| `tg_send_document` | `<profile> <chat_id> <file_path> [caption]` | Send a document/file |
| `tg_send_audio` | `<profile> <chat_id> <file_path> [duration] [title] [performer]` | Send an audio file |
| `tg_send_video` | `<profile> <chat_id> <file_path> [duration] [width] [height] [caption]` | Send a video |
| `tg_send_voice` | `<profile> <chat_id> [reply_to:<msg_id>] <file_path> [duration]` | Send a voice note |
| `tg_send_video_note` | `<profile> <chat_id> [reply_to:<msg_id>] <file_path> [duration] [length]` | Send a round video note |
| `tg_send_album` | `<profile> <chat_id> [reply_to:<msg_id>] <file1> [file2 ...]` | Send up to 10 photos/videos as an album |
| `tg_send_contact` | `<profile> <chat_id> <phone> <first_name> [last_name]` | Send a contact card |
| `tg_send_location` | `<profile> <chat_id> <latitude> <longitude>` | Send a map location |
| `tg_send_inline_result` | `<profile> <chat_id> <bot_username> [query]` | Query a bot inline and send its first result |

### Media — Inbound

Incoming files (photos, documents, audio, video, voice notes, video notes) are automatically downloaded to:

```
${DB_DIR}/telegram/<profile>/files/<type>/
```

A `TELEGRAM::INBOUND_FILE` custom event is fired on completion with headers:
`Telegram-Profile`, `Telegram-Chat-Id`, `Telegram-From-User-Id`, `Telegram-Message-Id`,
`Telegram-File-Type`, `Telegram-File-Path`, `Telegram-File-Size`, `Telegram-File-Name`,
`Telegram-Mime-Type`, `Telegram-Caption`, `Telegram-Duration`, `Telegram-Width`, `Telegram-Height`,
`Telegram-Audio-Title`, `Telegram-Audio-Performer`

Pending downloads survive restarts (persisted in `${DB_DIR}/mod_telegram.db`).

### Message Editing & Deletion

| Command | Arguments | Description |
|---|---|---|
| `tg_edit_message_text` | `<profile> <chat_id> <msg_id> <new_text>` | Edit the text of a sent message |
| `tg_edit_message_caption` | `<profile> <chat_id> <msg_id> <new_caption>` | Edit the caption of a photo/video/document |
| `tg_delete_messages` | `<profile> <chat_id> <msg_id1> [msg_id2 ...]` | Delete up to 20 messages for both sides |
| `tg_clear_chat_history` | `<profile> <chat_id>` | Delete all messages in a chat for both sides |

> **Note:** `tg_delete_messages` and `tg_edit_message_*` require the **server-confirmed** message ID.
> Outgoing messages get a temporary local ID first; the permanent ID is assigned by the server
> via `updateMessageSendSucceeded`. Use the ID from `TELEGRAM::OUTGOING_MESSAGE` events (when implemented)
> or capture it from logs (`updateMessageSendSucceeded … id = <server_id>`).

### Reactions

| Command | Arguments | Description |
|---|---|---|
| `tg_react` | `<profile> <chat_id> <msg_id> <emoji>` | Add a reaction to a message |

**Inbound reaction events:**

| Event | Key Headers |
|---|---|
| `TELEGRAM::REACTION` | `Telegram-Profile`, `Telegram-Chat-Id`, `Telegram-Message-Id`, `Telegram-Reaction-0-Emoji`, ... |
| `TELEGRAM::REACTION_COUNT` | `Telegram-Profile`, `Telegram-Chat-Id`, `Telegram-Reaction-Count` |

### Speech Recognition

| Command | Arguments | Description |
|---|---|---|
| `tg_recognize_speech` | `<profile> <chat_id> <msg_id>` | Request Telegram server-side ASR (requires Telegram Premium) |

Result arrives as `TELEGRAM::SPEECH_RECOGNITION` event with `Telegram-Text` header.

---

## Events Reference

| Event | Fired when |
|---|---|
| `TELEGRAM::INBOUND_FILE` | A downloaded file is complete |
| `TELEGRAM::SPEECH_RECOGNITION` | ASR result (or error) arrives |
| `TELEGRAM::REACTION` | A message receives a reaction |
| `TELEGRAM::REACTION_COUNT` | Chat-level unread reaction count changes |

---

## Message ID Notes

Telegram uses two message ID stages for outgoing messages:

1. **Local temporary ID** — assigned immediately on send (not divisible by 1,048,576). Cannot be used with `tg_delete_messages` or `tg_edit_message_*`.
2. **Server-confirmed ID** — assigned after Telegram server acknowledges (always divisible by 1,048,576). Use this with all edit/delete commands.

Inbound message IDs from `updateNewMessage` are already server-confirmed IDs.

---
