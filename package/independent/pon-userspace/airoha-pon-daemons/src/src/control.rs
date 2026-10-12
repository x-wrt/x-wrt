// SPDX-License-Identifier: GPL-2.0-only

use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::thread;

pub struct ControlServer {
    socket_path: PathBuf,
}

impl ControlServer {
    pub fn start<F>(socket_path: &Path, handler: F) -> io::Result<Self>
    where
        F: Fn(UnixStream) -> io::Result<()> + Send + Sync + 'static,
    {
        prepare_socket_path(socket_path)?;
        let listener = UnixListener::bind(socket_path)?;
        fs::set_permissions(socket_path, fs::Permissions::from_mode(0o600))?;

        thread::Builder::new()
            .name("pon-control".to_owned())
            .spawn(move || {
                for stream in listener.incoming().flatten() {
                    let _ = handler(stream);
                }
            })
            .map_err(io::Error::other)?;

        Ok(Self {
            socket_path: socket_path.to_owned(),
        })
    }
}

impl Drop for ControlServer {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.socket_path);
    }
}

fn prepare_socket_path(socket_path: &Path) -> io::Result<()> {
    if socket_path.exists() {
        fs::remove_file(socket_path)?;
    }
    Ok(())
}
