use super::Room;
use crate::{protocol::HubMsg, same_origin, user_of, App};
use axum::{
    body::Body,
    extract::{Path, State},
    http::{header, HeaderMap, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
};
use futures_util::StreamExt;
use std::{
    collections::HashSet,
    fs::{File, OpenOptions},
    io,
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    sync::Arc,
    time::Duration,
};
use tokio::{
    io::AsyncWriteExt,
    sync::{watch, OwnedSemaphorePermit},
    time::{sleep, timeout, timeout_at, Instant},
};
use tokio_util::io::ReaderStream;

const MAX_BYTES: usize = 32 * 1024 * 1024;
const TTL: Duration = Duration::from_secs(60);
const UPLOAD_TIMEOUT: Duration = Duration::from_secs(120);
const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(120);

type Watch = watch::Receiver<bool>;

pub(super) struct Offer {
    source: u64,
    source_lease: Watch,
    sharing: Watch,
    targets: Option<HashSet<u64>>,
    blob: Arc<Blob>,
}

struct Blob {
    file: File,
    size: usize,
    _slot: OwnedSemaphorePermit,
}

fn valid(lease: &Watch) -> bool {
    *lease.borrow() && matches!(lease.has_changed(), Ok(false))
}

fn id_is_valid(id: &str) -> bool {
    id.len() == 43 && id.bytes().all(|byte| byte.is_ascii_alphanumeric() || b"-_".contains(&byte))
}

impl Room {
    fn image_access(&mut self, headers: &HeaderMap) -> Option<(u64, Watch, Watch)> {
        if !self.snapshot.settings.clipboard {
            return None;
        }
        let id = headers.get("x-lzc-mesh-device-id")?.to_str().ok()?;
        let token = headers.get("x-lzc-mesh-device-token")?.to_str().ok()?;
        let lease = self.file_lease(id, token)?;
        let (&conn, _) = self.conns.iter().find(|(_, client)| {
            client.clipboard_image && client.device.as_deref() == Some(id)
                && self.trusted_conn(client)
        })?;
        Some((conn, lease, self.image_sharing.subscribe()))
    }

    pub(super) fn offer_image(&mut self, conn: u64, id: &str) {
        let Some(offer) = self.images.get(id) else { return };
        if offer.source != conn || offer.targets.is_some() || !valid(&offer.source_lease)
            || !valid(&offer.sharing)
        {
            return;
        }
        let Some(name) = self.conns.get(&conn)
            .and_then(|client| client.device.as_deref())
            .and_then(|device| self.snapshot.devices.get(device))
            .map(|device| device.name.clone())
        else { return };
        let message = HubMsg::ClipboardImage { id: id.to_owned(), size: offer.blob.size, from: name };
        let targets: HashSet<_> = self.conns.iter().filter_map(|(&target, client)| {
            if target != conn && client.clipboard_image && self.trusted_conn(client)
                && client.device.as_deref().is_some_and(|device| self.device_enabled(device))
                && client.tx.send(message.clone()).is_ok()
            {
                Some(target)
            } else {
                None
            }
        }).collect();
        if targets.is_empty() {
            self.images.remove(id);
        } else if let Some(offer) = self.images.get_mut(id) {
            offer.targets = Some(targets);
        }
    }

    pub(super) fn forget_image_connection(&mut self, conn: u64) {
        self.images.retain(|_, offer| {
            offer.source != conn && offer.targets.as_mut().is_none_or(|targets| {
                targets.remove(&conn);
                !targets.is_empty()
            })
        });
    }

    fn claim_image(&mut self, id: &str, conn: u64) -> Option<(Arc<Blob>, Watch)> {
        let offer = self.images.get_mut(id)?;
        if !valid(&offer.source_lease) || !valid(&offer.sharing) {
            return None;
        }
        let targets = offer.targets.as_mut()?;
        if !targets.remove(&conn) {
            return None;
        }
        let result = (offer.blob.clone(), offer.source_lease.clone());
        if targets.is_empty() {
            self.images.remove(id);
        }
        Some(result)
    }
}

fn png_dimensions(header: &[u8]) -> bool {
    let Some(header) = header.get(..24) else { return false };
    if &header[..16] != b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR" {
        return false;
    }
    let width = u32::from_be_bytes(header[16..20].try_into().unwrap());
    let height = u32::from_be_bytes(header[20..24].try_into().unwrap());
    width.min(height) > 0 && width.min(height) <= 4320 && width.max(height) <= 7680
}

fn temporary_file(id: &str) -> io::Result<File> {
    let path = std::env::temp_dir().join(format!("lzc-mesh-clipboard-{id}"));
    let file = OpenOptions::new().read(true).write(true).create_new(true).mode(0o600).open(&path)?;
    std::fs::remove_file(path)?;
    Ok(file)
}

async fn receive(file: File, body: Body, declared: Option<usize>) -> Result<(File, usize), StatusCode> {
    let mut file = tokio::fs::File::from_std(file);
    let mut chunks = body.into_data_stream();
    let mut prefix = Vec::with_capacity(24);
    let mut size = 0;
    while let Some(chunk) = chunks.next().await {
        let chunk = chunk.map_err(|_| StatusCode::BAD_REQUEST)?;
        if chunk.len() > MAX_BYTES - size {
            return Err(StatusCode::PAYLOAD_TOO_LARGE);
        }
        if prefix.len() < 24 {
            prefix.extend_from_slice(&chunk[..chunk.len().min(24 - prefix.len())]);
            if prefix.len() == 24 && !png_dimensions(&prefix) {
                return Err(StatusCode::UNPROCESSABLE_ENTITY);
            }
        }
        file.write_all(&chunk).await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        size += chunk.len();
    }
    if size < 45 || declared.is_some_and(|length| length != size) {
        return Err(StatusCode::BAD_REQUEST);
    }
    file.flush().await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    Ok((file.into_std().await, size))
}

pub(crate) async fn upload(State(app): State<App>, headers: HeaderMap, body: Body) -> Result<Response, StatusCode> {
    let user = user_of(&headers).ok_or(StatusCode::UNAUTHORIZED)?;
    if !same_origin(&headers) { return Err(StatusCode::FORBIDDEN); }
    let room = app.hub.room(&user).await;
    let (source, mut source_lease, mut sharing) = room.lock().await.image_access(&headers).ok_or(StatusCode::FORBIDDEN)?;
    if headers.get(header::CONTENT_TYPE).is_none_or(|value| value != "image/png") {
        return Err(StatusCode::UNSUPPORTED_MEDIA_TYPE);
    }
    let declared = headers.get(header::CONTENT_LENGTH).and_then(|value| value.to_str().ok()?.parse::<usize>().ok());
    if declared.is_some_and(|length| length > MAX_BYTES) {
        return Err(StatusCode::PAYLOAD_TOO_LARGE);
    }
    let slot = app.image_slots.try_acquire_owned().map_err(|_| StatusCode::TOO_MANY_REQUESTS)?;
    let id = crate::identity::nonce().map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?
        .replace('+', "-").replace('/', "_").trim_end_matches('=').to_owned();
    let file = temporary_file(&id).map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    let (file, size) = tokio::select! {
        biased;
        _ = source_lease.changed() => return Err(StatusCode::FORBIDDEN),
        _ = sharing.changed() => return Err(StatusCode::FORBIDDEN),
        result = timeout(UPLOAD_TIMEOUT, receive(file, body, declared)) => {
            result.map_err(|_| StatusCode::REQUEST_TIMEOUT)??
        }
    };
    {
        let mut room = room.lock().await;
        if !valid(&source_lease) || !valid(&sharing) {
            return Err(StatusCode::FORBIDDEN);
        }
        room.images.retain(|_, offer| offer.source != source || offer.targets.is_some());
        room.images.insert(id.clone(), Offer {
            source, source_lease, sharing, targets: None,
            blob: Arc::new(Blob { file, size, _slot: slot }),
        });
    }
    let expiry_id = id.clone();
    let room = Arc::downgrade(&room);
    tokio::spawn(async move {
        sleep(TTL).await;
        if let Some(room) = room.upgrade() {
            room.lock().await.images.remove(&expiry_id);
        }
    });
    Ok((StatusCode::CREATED, [(header::CACHE_CONTROL, "no-store")], id).into_response())
}

pub(crate) async fn download(State(app): State<App>, headers: HeaderMap, Path(id): Path<String>) -> Result<Response, StatusCode> {
    let user = user_of(&headers).ok_or(StatusCode::UNAUTHORIZED)?;
    if !same_origin(&headers) { return Err(StatusCode::FORBIDDEN); }
    if !id_is_valid(&id) { return Err(StatusCode::NOT_FOUND); }
    let room = app.hub.room(&user).await;
    let (target, sharing, blob, source) = {
        let mut room = room.lock().await;
        let (conn, lease, sharing) = room.image_access(&headers).ok_or(StatusCode::FORBIDDEN)?;
        let (blob, source) = room.claim_image(&id, conn).ok_or(StatusCode::NOT_FOUND)?;
        (lease, sharing, blob, source)
    };
    let file = tokio::fs::File::open(format!("/proc/self/fd/{}", blob.file.as_raw_fd()))
        .await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    let length = blob.size;
    let deadline = Instant::now() + DOWNLOAD_TIMEOUT;
    let stream = futures_util::stream::unfold(
        (ReaderStream::new(file), target, source, sharing, blob),
        move |(mut reader, mut target, mut source, mut sharing, blob)| async move {
            let item = tokio::select! {
                biased;
                _ = target.changed() => None,
                _ = source.changed() => None,
                _ = sharing.changed() => None,
                item = timeout_at(deadline, reader.next()) => item.unwrap_or(None),
            };
            item.map(|item| (item, (reader, target, source, sharing, blob)))
        },
    );
    let mut response = Body::from_stream(stream).into_response();
    let headers = response.headers_mut();
    headers.insert(header::CONTENT_LENGTH, HeaderValue::from(length));
    headers.insert(header::CONTENT_TYPE, HeaderValue::from_static("image/png"));
    headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    headers.insert("x-content-type-options", HeaderValue::from_static("nosniff"));
    Ok(response)
}
