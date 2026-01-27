use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use futures_util::{SinkExt, StreamExt};
use log::{debug, error, info, warn};
use serde::{Deserialize, Serialize};
use std::env;
use std::io::Result as IoResult;
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::protocol::Message;

const PROTO_VERSION: u8 = 0b0001;
const HEADER_SIZE_4B: u8 = 0b0001;
const MSG_FULL_CLIENT_REQUEST: u8 = 0b0001;
const MSG_AUDIO_ONLY_REQUEST: u8 = 0b0010;
const MSG_FULL_SERVER_RESPONSE: u8 = 0b1001;
const MSG_ERROR_RESPONSE: u8 = 0b1111;
const FLAG_NO_SEQUENCE: u8 = 0b0000;
const FLAG_LAST_NO_SEQUENCE: u8 = 0b0010;
const SERIALIZATION_JSON: u8 = 0b0001;
const SERIALIZATION_NONE: u8 = 0b0000;
const COMPRESSION_NONE: u8 = 0b0000;

#[derive(Debug, Deserialize)]
#[serde(tag = "type")]
enum ClientMsg {
    #[serde(rename = "start")]
    Start { mode: Option<String> },
    #[serde(rename = "stop")]
    Stop,
    #[serde(rename = "cancel")]
    Cancel,
    #[serde(other)]
    Other,
}

#[derive(Debug, Serialize)]
#[serde(tag = "type")]
enum ServerMsg<'a> {
    #[serde(rename = "status")]
    Status { state: &'a str },
    #[serde(rename = "partial")]
    Partial { text: &'a str },
    #[serde(rename = "final")]
    Final { text: &'a str },
    #[serde(rename = "error")]
    Error { message: &'a str },
}

#[derive(Clone, Debug)]
struct AsrConfig {
    app_id: String,
    access_token: String,
    resource_id: String,
    mode: String,
}

#[derive(Debug)]
enum AudioMsg {
    Chunk(Vec<u8>),
    End(Vec<u8>),
}

struct AudioWorker {
    stop: Arc<AtomicBool>,
    join: Option<std::thread::JoinHandle<()>>,
}

fn socket_path() -> PathBuf {
    if let Ok(dir) = env::var("XDG_RUNTIME_DIR") {
        if !dir.trim().is_empty() {
            return PathBuf::from(dir).join("anytalk.sock");
        }
    }
    if let Ok(uid) = env::var("UID") {
        if !uid.trim().is_empty() {
            return PathBuf::from("/run/user").join(uid).join("anytalk.sock");
        }
    }
    PathBuf::from("/tmp/anytalk.sock")
}

fn load_asr_config() -> Result<AsrConfig, String> {
    let app_id = env::var("ANYTALK_APP_ID")
        .map(|s| s.trim().to_string())
        .map_err(|_| "missing ANYTALK_APP_ID".to_string())?;
    let access_token = env::var("ANYTALK_ACCESS_TOKEN")
        .map(|s| s.trim().to_string())
        .map_err(|_| "missing ANYTALK_ACCESS_TOKEN".to_string())?;
    let resource_id = env::var("ANYTALK_RESOURCE_ID")
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "volc.seedasr.sauc.duration".to_string());
    let mode = env::var("ANYTALK_MODE")
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "bidi_async".to_string());

    info!(
        "Loaded Config: AppID={}, ResourceID={}, Mode={}",
        app_id, resource_id, mode
    );

    Ok(AsrConfig {
        app_id,
        access_token,
        resource_id,
        mode,
    })
}

fn asr_url(mode: &str) -> &'static str {
    match mode {
        "bidi" => "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel",
        "bidi_async" => "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
        _ => "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream",
    }
}

fn default_request_json(mode: &str) -> String {
    let is_nostream = mode == "nostream";
    let mut obj = serde_json::json!({
        "user": {"uid": "anytalk"},
        "audio": {
            "format": "pcm",
            "rate": 16000,
            "bits": 16,
            "channel": 1
        },
        "request": {
            "model_name": "bigmodel",
            "enable_itn": true,
            "enable_punc": true,
            "enable_ddc": false,
            "enable_word": false,
            "res_type": "full",
            "nbest": 1,
            "use_vad": true
        }
    });
    if is_nostream {
        if let Some(audio) = obj.get_mut("audio") {
            audio["language"] = serde_json::Value::String("zh-CN".to_string());
        }
    }
    obj.to_string()
}

fn build_header(message_type: u8, flags: u8, serialization: u8, compression: u8) -> [u8; 4] {
    let b0 = ((PROTO_VERSION & 0xF) << 4) | (HEADER_SIZE_4B & 0xF);
    let b1 = ((message_type & 0xF) << 4) | (flags & 0xF);
    let b2 = ((serialization & 0xF) << 4) | (compression & 0xF);
    [b0, b1, b2, 0x00]
}

fn u32be(n: usize) -> [u8; 4] {
    (n as u32).to_be_bytes()
}

fn build_full_client_request(payload_json_text: &str) -> Vec<u8> {
    let payload = payload_json_text.as_bytes();
    let mut out = Vec::with_capacity(4 + 4 + payload.len());
    let header = build_header(
        MSG_FULL_CLIENT_REQUEST,
        FLAG_NO_SEQUENCE,
        SERIALIZATION_JSON,
        COMPRESSION_NONE,
    );
    out.extend_from_slice(&header);
    out.extend_from_slice(&u32be(payload.len()));
    out.extend_from_slice(payload);
    out
}

fn build_audio_only_request(pcm_bytes: &[u8], last: bool) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + 4 + pcm_bytes.len());
    let header = build_header(
        MSG_AUDIO_ONLY_REQUEST,
        if last {
            FLAG_LAST_NO_SEQUENCE
        } else {
            FLAG_NO_SEQUENCE
        },
        SERIALIZATION_NONE,
        COMPRESSION_NONE,
    );
    out.extend_from_slice(&header);
    out.extend_from_slice(&u32be(pcm_bytes.len()));
    out.extend_from_slice(pcm_bytes);
    out
}

#[derive(Debug)]
struct ParsedServerMessage {
    kind: &'static str,
    flags: u8,
    json_text: Option<String>,
    error_code: Option<u32>,
    error_msg: Option<String>,
}

fn parse_server_message(data: &[u8]) -> ParsedServerMessage {
    if data.len() < 4 {
        return ParsedServerMessage {
            kind: "unknown",
            flags: 0,
            json_text: None,
            error_code: None,
            error_msg: None,
        };
    }

    let b0 = data[0];
    let b1 = data[1];
    let b2 = data[2];
    let version = (b0 >> 4) & 0xF;
    let header_size_4 = b0 & 0xF;
    if version != PROTO_VERSION || header_size_4 != HEADER_SIZE_4B {
        return ParsedServerMessage {
            kind: "unknown",
            flags: 0,
            json_text: None,
            error_code: None,
            error_msg: None,
        };
    }

    let message_type = (b1 >> 4) & 0xF;
    let flags = b1 & 0xF;
    let _compression = b2 & 0xF;

    if message_type == MSG_FULL_SERVER_RESPONSE {
        if data.len() < 12 {
            return ParsedServerMessage {
                kind: "unknown",
                flags,
                json_text: None,
                error_code: None,
                error_msg: None,
            };
        }
        let payload_size = u32::from_be_bytes([data[8], data[9], data[10], data[11]]) as usize;
        if data.len() < 12 + payload_size {
            return ParsedServerMessage {
                kind: "unknown",
                flags,
                json_text: None,
                error_code: None,
                error_msg: None,
            };
        }
        let payload = &data[12..12 + payload_size];
        let json_text = String::from_utf8_lossy(payload).to_string();
        return ParsedServerMessage {
            kind: "response",
            flags,
            json_text: Some(json_text),
            error_code: None,
            error_msg: None,
        };
    }

    if message_type == MSG_ERROR_RESPONSE {
        if data.len() < 12 {
            return ParsedServerMessage {
                kind: "unknown",
                flags,
                json_text: None,
                error_code: None,
                error_msg: None,
            };
        }
        let code = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
        let msg_size = u32::from_be_bytes([data[8], data[9], data[10], data[11]]) as usize;
        if data.len() < 12 + msg_size {
            return ParsedServerMessage {
                kind: "unknown",
                flags,
                json_text: None,
                error_code: None,
                error_msg: None,
            };
        }
        let msg = String::from_utf8_lossy(&data[12..12 + msg_size]).to_string();
        return ParsedServerMessage {
            kind: "error",
            flags,
            json_text: None,
            error_code: Some(code),
            error_msg: Some(msg),
        };
    }

    ParsedServerMessage {
        kind: "unknown",
        flags,
        json_text: None,
        error_code: None,
        error_msg: None,
    }
}

struct StreamingResampler {
    in_rate: usize,
    out_rate: usize,
    pos: f64,
    tail: Vec<i16>,
}

impl StreamingResampler {
    fn new(in_rate: usize, out_rate: usize) -> Self {
        Self {
            in_rate,
            out_rate,
            pos: 0.0,
            tail: Vec::new(),
        }
    }

    fn process(&mut self, input: &[i16]) -> Vec<i16> {
        if self.in_rate == self.out_rate {
            return input.to_vec();
        }
        if input.is_empty() {
            return Vec::new();
        }
        let mut merged = Vec::with_capacity(self.tail.len() + input.len());
        merged.extend_from_slice(&self.tail);
        merged.extend_from_slice(input);

        let step = self.in_rate as f64 / self.out_rate as f64;
        let mut out = Vec::new();
        loop {
            let i0 = self.pos.floor() as usize;
            let i1 = i0 + 1;
            if i1 >= merged.len() {
                break;
            }
            let frac = self.pos - i0 as f64;
            let v0 = merged[i0] as f64;
            let v1 = merged[i1] as f64;
            let v = v0 * (1.0 - frac) + v1 * frac;
            let v = v.round().clamp(-32768.0, 32767.0) as i16;
            out.push(v);
            self.pos += step;
        }

        let base = self.pos.floor() as usize;
        let keep_from = base.saturating_sub(1);
        self.tail = merged[keep_from..].to_vec();
        self.pos -= keep_from as f64;
        out
    }
}

fn i16_to_le_bytes(samples: &[i16]) -> Vec<u8> {
    let mut out = Vec::with_capacity(samples.len() * 2);
    for s in samples {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

fn start_audio_worker() -> Result<(AudioWorker, mpsc::Receiver<AudioMsg>), String> {
    let host = cpal::default_host();
    let device = host
        .default_input_device()
        .ok_or_else(|| "no input device".to_string())?;
    let device_name = device.name().unwrap_or_else(|_| "unknown".to_string());
    info!("Using default input device: {}", device_name);

    let config = device
        .default_input_config()
        .map_err(|e| format!("input config error: {e}"))?;
    info!("Default input config: {:?}", config);

    let channels = config.channels() as usize;
    let in_rate = config.sample_rate().0 as usize;
    let stop = Arc::new(AtomicBool::new(false));
    let (tx, rx) = mpsc::channel(16);
    let stop_for_thread = stop.clone();

    let join = std::thread::spawn(move || {
        let stop_flag = stop_for_thread.clone();
        let buffer = Arc::new(std::sync::Mutex::new(Vec::<i16>::new()));
        let mut resampler = StreamingResampler::new(in_rate, 16000);
        let chunk_samples = 16000 * 200 / 1000;

        let err_fn = |err| error!("audio error: {err}");
        let tx_audio = tx.clone();
        let stop_for_stream = stop_flag.clone();
        let buffer_for_stream = Arc::clone(&buffer);
        let tx_for_stream = tx_audio.clone();

        let stream = match config.sample_format() {
            cpal::SampleFormat::F32 => device
                .build_input_stream(
                    &config.into(),
                    move |data: &[f32], _| {
                        if stop_for_stream.load(Ordering::Relaxed) {
                            return;
                        }
                        let mut buffer = buffer_for_stream.lock().unwrap();
                        let mut samples: Vec<i16> = Vec::with_capacity(data.len());
                        for &s in data {
                            let v = (s.clamp(-1.0, 1.0) * 32767.0).round() as i16;
                            samples.push(v);
                        }
                        push_samples(
                            &mut buffer,
                            &mut resampler,
                            channels,
                            &samples,
                            chunk_samples,
                            &tx_for_stream,
                        );
                    },
                    err_fn,
                    None,
                )
                .ok(),
            cpal::SampleFormat::I16 => device
                .build_input_stream(
                    &config.into(),
                    move |data: &[i16], _| {
                        if stop_for_stream.load(Ordering::Relaxed) {
                            return;
                        }
                        let mut buffer = buffer_for_stream.lock().unwrap();
                        push_samples(
                            &mut buffer,
                            &mut resampler,
                            channels,
                            data,
                            chunk_samples,
                            &tx_for_stream,
                        );
                    },
                    err_fn,
                    None,
                )
                .ok(),
            cpal::SampleFormat::U16 => device
                .build_input_stream(
                    &config.into(),
                    move |data: &[u16], _| {
                        if stop_for_stream.load(Ordering::Relaxed) {
                            return;
                        }
                        let mut buffer = buffer_for_stream.lock().unwrap();
                        let mut samples: Vec<i16> = Vec::with_capacity(data.len());
                        for &s in data {
                            samples.push(((s as i32) - 32768) as i16);
                        }
                        push_samples(
                            &mut buffer,
                            &mut resampler,
                            channels,
                            &samples,
                            chunk_samples,
                            &tx_for_stream,
                        );
                    },
                    err_fn,
                    None,
                )
                .ok(),
            _ => None,
        };

        if let Some(stream) = stream {
            if stream.play().is_err() {
                eprintln!("audio play error");
            }
            while !stop_flag.load(Ordering::Relaxed) {
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            let remainder = buffer.lock().unwrap().clone();
            let bytes = i16_to_le_bytes(&remainder);
            let _ = tx_audio.blocking_send(AudioMsg::End(bytes));
        } else {
            eprintln!("unsupported sample format");
        }
    });

    Ok((
        AudioWorker {
            stop,
            join: Some(join),
        },
        rx,
    ))
}

fn push_samples(
    buffer: &mut Vec<i16>,
    resampler: &mut StreamingResampler,
    channels: usize,
    input: &[i16],
    chunk_samples: usize,
    tx: &mpsc::Sender<AudioMsg>,
) {
    if input.is_empty() {
        return;
    }
    let mut mono: Vec<i16> = Vec::new();
    if channels <= 1 {
        mono.extend_from_slice(input);
    } else {
        for frame in input.chunks(channels) {
            let sum: i32 = frame.iter().map(|v| *v as i32).sum();
            let avg = (sum / frame.len() as i32) as i16;
            mono.push(avg);
        }
    }
    let resampled = resampler.process(&mono);
    buffer.extend_from_slice(&resampled);
    while buffer.len() >= chunk_samples {
        let chunk = buffer.drain(..chunk_samples).collect::<Vec<_>>();
        let bytes = i16_to_le_bytes(&chunk);
        let _ = tx.blocking_send(AudioMsg::Chunk(bytes));
    }
}

async fn run_ws(
    mut audio_rx: mpsc::Receiver<AudioMsg>,
    resp_tx: mpsc::Sender<String>,
    cfg: AsrConfig,
) -> Result<(), String> {
    let url = asr_url(&cfg.mode);
    info!("Connecting to ASR URL: {}", url);
    let mut request = url
        .into_client_request()
        .map_err(|e| format!("ws request error: {e}"))?;
    {
        let headers = request.headers_mut();
        headers.insert(
            "X-Api-App-Key",
            cfg.app_id.parse().map_err(|_| "bad app id")?,
        );
        headers.insert(
            "X-Api-Access-Key",
            cfg.access_token.parse().map_err(|_| "bad access token")?,
        );
        headers.insert(
            "X-Api-Resource-Id",
            cfg.resource_id.parse().map_err(|_| "bad resource id")?,
        );
        headers.insert(
            "X-Api-Connect-Id",
            uuid::Uuid::new_v4()
                .to_string()
                .parse()
                .map_err(|_| "bad uuid")?,
        );
    }

    let (ws_stream, _) = tokio_tungstenite::connect_async(request)
        .await
        .map_err(|e| format!("ws connect error: {e}"))?;
    info!("WebSocket connected");
    let (mut ws_write, mut ws_read) = ws_stream.split();

    let req_json = default_request_json(&cfg.mode);
    debug!("Sending initial request: {}", req_json);
    let frame = build_full_client_request(&req_json);
    ws_write
        .send(Message::Binary(frame))
        .await
        .map_err(|e| format!("ws send error: {e}"))?;

    let mut last_committed_end_time: i64 = -1;

    let mut last_full_text = String::new();

    let mut chunk_count = 0;

    let mut audio_active = true;

    loop {
        tokio::select! {

            audio = audio_rx.recv(), if audio_active => {

                match audio {

                    Some(AudioMsg::Chunk(bytes)) => {

                        chunk_count += 1;

                        if chunk_count % 20 == 0 {

                            debug!("Sent 20 audio chunks to ASR...");

                        }

                        let frame = build_audio_only_request(&bytes, false);

                        if ws_write.send(Message::Binary(frame)).await.is_err() {

                            audio_active = false;

                        }

                    }

                    Some(AudioMsg::End(bytes)) => {

                        debug!("Sending final audio chunk");

                        let frame = build_audio_only_request(&bytes, true);

                        let _ = ws_write.send(Message::Binary(frame)).await;

                        audio_active = false;

                    }

                    None => {

                        debug!("Audio source channel closed");

                        audio_active = false;

                    }

                }

            }

            msg = ws_read.next() => {

                match msg {

                    Some(Ok(Message::Binary(data))) => {

                        let parsed = parse_server_message(&data);

                        if parsed.kind == "error" {

                            let msg = parsed.error_msg.unwrap_or_else(|| "server error".to_string());

                            error!("ASR Error: {}", msg);

                            let _ = resp_tx.send(serialize_msg(ServerMsg::Error { message: &msg })).await;

                            break;

                        }

                        if parsed.kind != "response" {

                            continue;

                        }

                        if let Some(json_text) = parsed.json_text {

                            debug!("ASR Response (flags={:b}): {}", parsed.flags, json_text);

                            let (partial, finals) = parse_asr_texts(&json_text, &mut last_committed_end_time, &mut last_full_text, cfg.mode.as_str());

                            if let Some(p) = partial {

                                let _ = resp_tx.send(serialize_msg(ServerMsg::Partial { text: &p })).await;

                            }

                            for f in finals {

                                debug!("Committing final text: {}", f);

                                let _ = resp_tx.send(serialize_msg(ServerMsg::Final { text: &f })).await;

                            }

                            // 0b0011 means this is the final response frame from server

                            if parsed.flags == 0b0011 {

                                info!("Received final server response frame. Closing.");

                                break;

                            }

                        }

                    }

                    Some(Ok(Message::Close(_))) => {

                        info!("WebSocket closed by server");

                        break;

                    }

                    Some(Ok(_)) => {},

                    Some(Err(e)) => {

                        error!("WebSocket error: {}", e);

                        break;

                    }

                    None => {

                        debug!("WebSocket stream ended (None)");

                        break;

                    }

                }

            }

        }
    }

    Ok(())
}

fn parse_asr_texts(
    json_text: &str,
    last_committed_end_time: &mut i64,
    last_full_text: &mut String,
    mode: &str,
) -> (Option<String>, Vec<String>) {
    let mut partial: Option<String> = None;
    let mut finals: Vec<String> = Vec::new();

    let obj: serde_json::Value = match serde_json::from_str(json_text) {
        Ok(v) => v,
        Err(_) => return (None, finals),
    };
    let result = match obj.get("result") {
        Some(r) => r,
        None => return (None, finals),
    };

    if let Some(utterances) = result.get("utterances").and_then(|u| u.as_array()) {
        for u in utterances {
            let def = u.get("definite").and_then(|v| v.as_bool()).unwrap_or(false);
            if !def {
                continue;
            }
            let end_time = u.get("end_time").and_then(|v| v.as_i64()).unwrap_or(-1);
            if end_time <= *last_committed_end_time {
                debug!(
                    "Skipping definite utterance: end_time {} <= last {}",
                    end_time, last_committed_end_time
                );
                continue;
            }
            if let Some(txt) = u.get("text").and_then(|v| v.as_str()) {
                let trimmed = txt.trim();
                if !trimmed.is_empty() {
                    debug!("New final: {} (end_time {})", trimmed, end_time);
                    finals.push(trimmed.to_string());
                    *last_committed_end_time = end_time;
                }
            }
        }
        for u in utterances.iter().rev() {
            if u.get("definite").and_then(|v| v.as_bool()).unwrap_or(false) {
                continue;
            }
            if let Some(txt) = u.get("text").and_then(|v| v.as_str()) {
                let trimmed = txt.trim();
                if !trimmed.is_empty() {
                    partial = Some(trimmed.to_string());
                    break;
                }
            }
        }
        return (partial, finals);
    }

    if let Some(txt) = result.get("text").and_then(|v| v.as_str()) {
        let full = txt.trim().to_string();
        if full.is_empty() {
            return (None, finals);
        }
        if mode == "bidi_async" {
            partial = Some(full.clone());
            finals.push(full.clone());
        } else if !last_full_text.is_empty() && full.starts_with(last_full_text.as_str()) {
            let suffix = full[last_full_text.len()..].trim();
            if !suffix.is_empty() {
                finals.push(suffix.to_string());
            }
        } else if full != *last_full_text {
            finals.push(full.clone());
        }
        *last_full_text = full;
    }

    (partial, finals)
}

fn serialize_msg(msg: ServerMsg<'_>) -> String {
    let mut line = serde_json::to_string(&msg).unwrap_or_else(|_| "{}".to_string());
    line.push('\n');
    line
}

async fn handle_client(stream: UnixStream) -> IoResult<()> {
    let (read_half, mut write_half) = stream.into_split();
    let mut reader = BufReader::new(read_half).lines();

    let (resp_tx, mut resp_rx) = mpsc::channel::<String>(32);

    let mut session: Option<(AudioWorker, tokio::task::JoinHandle<()>)> = None;

    info!("New client connected");

    loop {
        tokio::select! {
            line = reader.next_line() => {
                let line = match line? {
                    Some(l) => l,
                    None => {
                        info!("Client disconnected");
                        break;
                    }
                };
                debug!("Received: {}", line);
                let msg: ClientMsg = serde_json::from_str(&line).unwrap_or(ClientMsg::Other);
                match msg {
                    ClientMsg::Start { .. } => {
                        info!("Received Start command");
                        if session.is_some() {
                            warn!("Session already active");
                            continue;
                        }
                        let cfg = match load_asr_config() {
                            Ok(v) => v,
                            Err(err) => {
                                error!("Config error: {}", err);
                                let _ = write_half.write_all(serialize_msg(ServerMsg::Error { message: &err }).as_bytes()).await;
                                continue;
                            }
                        };
                        let _ = write_half.write_all(serialize_msg(ServerMsg::Status { state: "connecting" }).as_bytes()).await;
                        let (audio_worker, audio_rx) = match start_audio_worker() {
                            Ok(v) => v,
                            Err(err) => {
                                error!("Audio worker error: {}", err);
                                let _ = write_half.write_all(serialize_msg(ServerMsg::Error { message: &err }).as_bytes()).await;
                                continue;
                            }
                        };
                        let resp_tx_clone = resp_tx.clone();
                        let ws_task = tokio::spawn(async move {
                            if let Err(e) = run_ws(audio_rx, resp_tx_clone.clone(), cfg).await {
                                error!("run_ws error: {}", e);
                                let _ = resp_tx_clone
                                    .send(serialize_msg(ServerMsg::Error { message: &e }))
                                    .await;
                            }
                            let _ = resp_tx_clone
                                .send(serialize_msg(ServerMsg::Status { state: "idle" }))
                                .await;
                        });
                        session = Some((audio_worker, ws_task));
                        let _ = write_half.write_all(serialize_msg(ServerMsg::Status { state: "recording" }).as_bytes()).await;
                    }
                    ClientMsg::Stop | ClientMsg::Cancel => {
                        info!("Received Stop/Cancel command");
                        if let Some((mut worker, ws_task)) = session.take() {
                            worker.stop.store(true, Ordering::Relaxed);
                            if let Some(join) = worker.join.take() {
                                let _ = join.join();
                            }
                            let _ = ws_task.await;
                        }
                        let _ = write_half.write_all(serialize_msg(ServerMsg::Status { state: "idle" }).as_bytes()).await;
                    }
                    ClientMsg::Other => {
                        warn!("Received unknown message");
                        let _ = write_half.write_all(serialize_msg(ServerMsg::Error { message: "unknown message" }).as_bytes()).await;
                    }
                }
            }
            resp = resp_rx.recv() => {
                if let Some(line) = resp {
                    debug!("Sending to client: {}", line.trim());
                    let _ = write_half.write_all(line.as_bytes()).await;
                }
            }
        }
    }

    Ok(())
}

#[tokio::main]

async fn main() -> IoResult<()> {

    env_logger::init();

    rustls::crypto::ring::default_provider().install_default().expect("Failed to install rustls crypto provider");



    let path = socket_path();

    

    // Check if another instance is running

    if path.exists() {

        match UnixStream::connect(&path).await {

            Ok(_) => {

                error!("Another instance of anytalk-daemon is already running.");

                return Ok(());

            }

            Err(_) => {

                warn!("Removing stale socket file: {}", path.display());

                let _ = std::fs::remove_file(&path);

            }

        }

    }



    let listener = UnixListener::bind(&path)?;

    info!("anytalk-daemon listening on {}", path.display());



    loop {
        let (stream, _) = listener.accept().await?;
        tokio::spawn(async move {
            if let Err(err) = handle_client(stream).await {
                error!("client error: {err}");
            }
        });
    }
}
