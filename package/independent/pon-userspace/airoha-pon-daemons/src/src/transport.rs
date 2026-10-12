// SPDX-License-Identifier: GPL-2.0-only

use std::ffi::CString;
use std::io;
use std::mem::{size_of, zeroed};
use std::os::raw::{c_char, c_int, c_uint, c_ulong, c_void};
use std::time::Duration;

const AF_PACKET: c_int = 17;
const SOCK_RAW: c_int = 3;
const ETH_P_ALL: u16 = 0x0003;
const SOL_PACKET: c_int = 263;
const PACKET_IGNORE_OUTGOING: c_int = 23;
const ENETDOWN: i32 = 100;
const POLLIN: i16 = 0x0001;
pub const PACKET_OUTGOING: u8 = 4;

type SockLen = u32;

pub fn line_is_down(error: &io::Error) -> bool {
    error.raw_os_error() == Some(ENETDOWN)
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SockAddrLl {
    sll_family: u16,
    sll_protocol: u16,
    sll_ifindex: i32,
    sll_hatype: u16,
    sll_pkttype: u8,
    sll_halen: u8,
    sll_addr: [u8; 8],
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

#[repr(C)]
struct SockAddr {
    sa_family: u16,
    sa_data: [u8; 14],
}

extern "C" {
    fn socket(domain: c_int, socket_type: c_int, protocol: c_int) -> c_int;
    fn bind(fd: c_int, address: *const SockAddr, length: SockLen) -> c_int;
    fn recvfrom(
        fd: c_int,
        buffer: *mut c_void,
        length: usize,
        flags: c_int,
        address: *mut SockAddr,
        address_length: *mut SockLen,
    ) -> isize;
    fn sendto(
        fd: c_int,
        buffer: *const c_void,
        length: usize,
        flags: c_int,
        address: *const SockAddr,
        address_length: SockLen,
    ) -> isize;
    fn setsockopt(
        fd: c_int,
        level: c_int,
        option_name: c_int,
        option_value: *const c_void,
        option_length: SockLen,
    ) -> c_int;
    fn poll(fds: *mut PollFd, count: c_ulong, timeout: c_int) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn if_nametoindex(name: *const c_char) -> c_uint;
}

pub struct ReceivedFrame<'a> {
    pub bytes: &'a [u8],
    pub packet_type: u8,
}

pub struct PacketSocket {
    fd: c_int,
    name: CString,
    address: SockAddrLl,
}

impl PacketSocket {
    pub fn open(interface: &str) -> io::Result<Self> {
        let name = CString::new(interface)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "interface contains NUL"))?;
        let ifindex = unsafe { if_nametoindex(name.as_ptr()) };
        if ifindex == 0 {
            return Err(io::Error::last_os_error());
        }

        let protocol = ETH_P_ALL.to_be();
        let fd = unsafe { socket(AF_PACKET, SOCK_RAW, protocol as c_int) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        let address = SockAddrLl {
            sll_family: AF_PACKET as u16,
            sll_protocol: protocol,
            sll_ifindex: ifindex as i32,
            sll_hatype: 0,
            sll_pkttype: 0,
            sll_halen: 0,
            sll_addr: [0; 8],
        };
        let result = unsafe {
            bind(
                fd,
                &address as *const SockAddrLl as *const SockAddr,
                size_of::<SockAddrLl>() as SockLen,
            )
        };
        if result < 0 {
            let error = io::Error::last_os_error();
            unsafe {
                close(fd);
            }
            return Err(error);
        }

        // PACKET_IGNORE_OUTGOING filters copies of frames emitted by this socket.
        let enabled: c_int = 1;
        unsafe {
            setsockopt(
                fd,
                SOL_PACKET,
                PACKET_IGNORE_OUTGOING,
                &enabled as *const c_int as *const c_void,
                size_of::<c_int>() as SockLen,
            );
        }

        Ok(Self { fd, name, address })
    }

    // AF_PACKET reports ENETDOWN once for both administrative down and
    // unregistration; only the former re-arms the socket on the next NETDEV_UP.
    pub fn interface_present(&self) -> bool {
        let ifindex = unsafe { if_nametoindex(self.name.as_ptr()) };
        ifindex != 0 && ifindex as i32 == self.address.sll_ifindex
    }

    /// Returns false when `timeout` passes without a frame or a pending socket error.
    pub fn wait_readable(&self, timeout: Duration) -> io::Result<bool> {
        let mut entry = PollFd {
            fd: self.fd,
            events: POLLIN,
            revents: 0,
        };
        let timeout = timeout.as_millis().min(c_int::MAX as u128) as c_int;
        let ready = unsafe { poll(&mut entry, 1, timeout) };
        if ready < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(ready > 0)
    }

    pub fn receive<'a>(&self, buffer: &'a mut [u8]) -> io::Result<ReceivedFrame<'a>> {
        let mut peer: SockAddrLl = unsafe { zeroed() };
        let mut peer_length = size_of::<SockAddrLl>() as SockLen;
        let received = unsafe {
            recvfrom(
                self.fd,
                buffer.as_mut_ptr() as *mut c_void,
                buffer.len(),
                0,
                &mut peer as *mut SockAddrLl as *mut SockAddr,
                &mut peer_length,
            )
        };
        if received < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(ReceivedFrame {
            bytes: &buffer[..received as usize],
            packet_type: peer.sll_pkttype,
        })
    }

    pub fn send(&self, frame: &[u8]) -> io::Result<()> {
        let sent = unsafe {
            sendto(
                self.fd,
                frame.as_ptr() as *const c_void,
                frame.len(),
                0,
                &self.address as *const SockAddrLl as *const SockAddr,
                size_of::<SockAddrLl>() as SockLen,
            )
        };
        if sent < 0 {
            return Err(io::Error::last_os_error());
        }
        if sent as usize != frame.len() {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                format!("partial AF_PACKET send: {sent}/{}", frame.len()),
            ));
        }
        Ok(())
    }
}

impl Drop for PacketSocket {
    fn drop(&mut self) {
        unsafe {
            close(self.fd);
        }
    }
}
