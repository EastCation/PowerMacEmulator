//! Rust userspace network backend for the PowerMac emulator.
//!
//! This crate exposes a small C ABI around `pktkit`'s userspace NAT stack. The
//! emulator feeds Ethernet frames in with `slirp_native_input` and drains guest
//! replies from `slirp_native_recv` on the emulator thread.  Gateway ARP and
//! DHCP are terminated here; TCP/UDP are forwarded to host sockets by pktkit.

use pktkit::dhcp::{Server, ServerConfig};
use pktkit::slirp::Stack;
use pktkit::{EtherType, Frame, IpPrefix, L2Adapter, L2AdapterConfig, L2Device, L3Device, MacAddr};
use std::collections::VecDeque;
use std::net::{IpAddr, Ipv4Addr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::{Arc, Mutex};

const GATEWAY: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 2);
const GUEST_IP: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 15);
const NETMASK: u8 = 24;
const DEVICE_MAC: MacAddr = MacAddr([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02]);
const FALLBACK_DNS: [Ipv4Addr; 2] = [Ipv4Addr::new(1, 1, 1, 1), Ipv4Addr::new(8, 8, 8, 8)];
const MAX_QUEUED_FRAMES: usize = 1024;

type OutputQueue = Arc<Mutex<VecDeque<Vec<u8>>>>;

/// Opaque handle returned to the C++ side.
pub struct SlirpNative {
    stack: Arc<Stack>,
    adapter: Arc<L2Adapter>,
    dhcp: Arc<Server>,
    output: OutputQueue,
}

impl SlirpNative {
    fn new(dns: Vec<Ipv4Addr>, mtu: u32) -> Option<Self> {
        let stack = Stack::new();
        stack.set_mtu(mtu.max(576)).ok()?;
        L3Device::set_addr(stack.as_ref(), IpPrefix::new(IpAddr::V4(GATEWAY), NETMASK)).ok()?;

        let adapter = L2Adapter::new_arc(
            stack.clone(),
            L2AdapterConfig::default()
                .mac(DEVICE_MAC)
                .gateway_v4(GATEWAY),
        );

        let output: OutputQueue = Arc::new(Mutex::new(VecDeque::new()));
        adapter.set_handler(queue_handler(output.clone()));

        let mut dhcp_cfg = ServerConfig::new(GATEWAY, GUEST_IP, GUEST_IP);
        dhcp_cfg.router = Some(GATEWAY);
        dhcp_cfg.dns = if dns.is_empty() {
            FALLBACK_DNS.to_vec()
        } else {
            dns
        };
        dhcp_cfg.mac = DEVICE_MAC;
        let dhcp = Arc::new(Server::new(dhcp_cfg));
        dhcp.set_handler(queue_handler(output.clone()));

        Some(Self {
            stack,
            adapter,
            dhcp,
            output,
        })
    }

    fn input(&self, bytes: &[u8]) {
        let frame = Frame::from_slice(bytes);
        if !frame.is_valid() {
            return;
        }

        // DHCP requests are terminated by the local server, not the NAT stack.
        if is_dhcp_request(frame) {
            let _ = self.dhcp.send(frame);
        } else {
            let _ = self.adapter.send(frame);
        }
    }

    fn recv(&self, out: &mut [u8]) -> Option<usize> {
        let mut queue = self
            .output
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let frame = queue.pop_front()?;
        if frame.len() > out.len() {
            return Some(0);
        }
        out[..frame.len()].copy_from_slice(&frame);
        Some(frame.len())
    }

    fn shutdown(&self) {
        let _ = self.adapter.close();
        let _ = self.dhcp.close();
        let _ = self.stack.shutdown();
    }
}

fn queue_handler(output: OutputQueue) -> pktkit::L2Handler {
    Arc::new(move |frame: &Frame| {
        let mut queue = output
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if queue.len() >= MAX_QUEUED_FRAMES {
            queue.pop_front();
        }
        queue.push_back(frame.to_vec());
        Ok(())
    })
}

fn is_dhcp_request(frame: &Frame) -> bool {
    if frame.ether_type() != EtherType::IPV4 {
        return false;
    }
    let packet = frame.payload();
    if packet.len() < 28 || packet[0] >> 4 != 4 {
        return false;
    }
    let header_len = usize::from(packet[0] & 0x0f) * 4;
    if header_len < 20 || packet.len() < header_len + 8 || packet[9] != 17 {
        return false;
    }
    let src_port = u16::from_be_bytes([packet[header_len], packet[header_len + 1]]);
    let dst_port = u16::from_be_bytes([packet[header_len + 2], packet[header_len + 3]]);
    src_port == 68 && dst_port == 67
}

fn parse_dns(dns: *const u8, count: usize) -> Vec<Ipv4Addr> {
    if dns.is_null() || count == 0 {
        return Vec::new();
    }
    let Some(len) = count.checked_mul(4) else {
        return Vec::new();
    };
    if len > 64 {
        return Vec::new();
    }
    let bytes = unsafe { std::slice::from_raw_parts(dns, len) };
    bytes
        .chunks_exact(4)
        .map(|octets| Ipv4Addr::new(octets[0], octets[1], octets[2], octets[3]))
        .collect()
}

/// Create a userspace network. `dns` points to `dns_count` IPv4 addresses in
/// network byte order. Returns null on failure.
#[no_mangle]
pub extern "C" fn slirp_native_create(
    dns: *const u8,
    dns_count: usize,
    mtu: u32,
) -> *mut SlirpNative {
    let servers = parse_dns(dns, dns_count);
    match catch_unwind(AssertUnwindSafe(|| SlirpNative::new(servers, mtu))) {
        Ok(Some(net)) => Box::into_raw(Box::new(net)),
        _ => ptr::null_mut(),
    }
}

/// Destroy a userspace network and stop its background threads.
#[no_mangle]
pub unsafe extern "C" fn slirp_native_destroy(net: *mut SlirpNative) {
    if net.is_null() {
        return;
    }
    let net = Box::from_raw(net);
    net.shutdown();
}

/// Feed one Ethernet frame from the guest. Returns 0 on success.
#[no_mangle]
pub unsafe extern "C" fn slirp_native_input(
    net: *mut SlirpNative,
    frame: *const u8,
    len: usize,
) -> i32 {
    if net.is_null() || frame.is_null() || len == 0 {
        return -1;
    }
    let net = &*net;
    let bytes = std::slice::from_raw_parts(frame, len);
    catch_unwind(AssertUnwindSafe(|| {
        net.input(bytes);
        0
    }))
    .unwrap_or(-2)
}

/// Pop one Ethernet frame for the guest. Returns the frame length, 0 when the
/// queue is empty or the buffer is too small, or a negative error code.
#[no_mangle]
pub unsafe extern "C" fn slirp_native_recv(net: *mut SlirpNative, out: *mut u8, cap: usize) -> i32 {
    if net.is_null() || out.is_null() || cap == 0 {
        return -1;
    }
    let net = &*net;
    let out = std::slice::from_raw_parts_mut(out, cap);
    catch_unwind(AssertUnwindSafe(|| match net.recv(out) {
        Some(len) => i32::try_from(len).unwrap_or(-3),
        None => 0,
    }))
    .unwrap_or(-2)
}

#[cfg(test)]
mod tests {
    use super::*;

    const GUEST_MAC: [u8; 6] = [0x02, 0x00, 0x00, 0x00, 0x00, 0x15];

    fn checksum(data: &[u8]) -> u16 {
        let mut sum = 0u32;
        for pair in data.chunks(2) {
            let word = if pair.len() == 2 {
                u16::from_be_bytes([pair[0], pair[1]])
            } else {
                u16::from_be_bytes([pair[0], 0])
            };
            sum += u32::from(word);
        }
        while sum >> 16 != 0 {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        !(sum as u16)
    }

    fn ethernet(dst: [u8; 6], src: [u8; 6], ether_type: u16, payload: &[u8]) -> Vec<u8> {
        let mut frame = Vec::with_capacity(14 + payload.len());
        frame.extend_from_slice(&dst);
        frame.extend_from_slice(&src);
        frame.extend_from_slice(&ether_type.to_be_bytes());
        frame.extend_from_slice(payload);
        frame
    }

    fn arp_request() -> Vec<u8> {
        let mut arp = Vec::with_capacity(28);
        arp.extend_from_slice(&1u16.to_be_bytes());
        arp.extend_from_slice(&0x0800u16.to_be_bytes());
        arp.push(6);
        arp.push(4);
        arp.extend_from_slice(&1u16.to_be_bytes());
        arp.extend_from_slice(&GUEST_MAC);
        arp.extend_from_slice(&GUEST_IP.octets());
        arp.extend_from_slice(&[0u8; 6]);
        arp.extend_from_slice(&GATEWAY.octets());
        ethernet([0xff; 6], GUEST_MAC, 0x0806, &arp)
    }

    fn dhcp_discover() -> Vec<u8> {
        let mut bootp = vec![0u8; 236];
        bootp[0] = 1;
        bootp[1] = 1;
        bootp[2] = 6;
        bootp[4..8].copy_from_slice(&0x1234_5678u32.to_be_bytes());
        bootp[10..12].copy_from_slice(&0x8000u16.to_be_bytes());
        bootp[28..34].copy_from_slice(&GUEST_MAC);
        bootp.extend_from_slice(&[0x63, 0x82, 0x53, 0x63]);
        bootp.extend_from_slice(&[53, 1, 1, 255]);

        let mut udp = Vec::with_capacity(8 + bootp.len());
        udp.extend_from_slice(&68u16.to_be_bytes());
        udp.extend_from_slice(&67u16.to_be_bytes());
        udp.extend_from_slice(&(bootp.len() as u16 + 8).to_be_bytes());
        udp.extend_from_slice(&[0, 0]);
        udp.extend_from_slice(&bootp);
        let pseudo = {
            let mut p = Vec::new();
            p.extend_from_slice(&[0u8; 4]);
            p.extend_from_slice(&[255u8; 4]);
            p.push(0);
            p.push(17);
            p.extend_from_slice(&(udp.len() as u16).to_be_bytes());
            p
        };
        let mut sum_data = pseudo;
        sum_data.extend_from_slice(&udp);
        let sum = checksum(&sum_data).to_be_bytes();
        udp[6..8].copy_from_slice(&sum);

        let mut ip = vec![0u8; 20];
        ip[0] = 0x45;
        ip[2..4].copy_from_slice(&((20 + udp.len()) as u16).to_be_bytes());
        ip[8] = 64;
        ip[9] = 17;
        ip[12..16].copy_from_slice(&[0u8; 4]);
        ip[16..20].copy_from_slice(&[255u8; 4]);
        let ip_sum = checksum(&ip).to_be_bytes();
        ip[10..12].copy_from_slice(&ip_sum);
        ip.extend_from_slice(&udp);

        ethernet([0xff; 6], GUEST_MAC, 0x0800, &ip)
    }

    fn recv_frame(net: &SlirpNative) -> Vec<u8> {
        let mut buf = vec![0u8; 2048];
        let len = net.recv(&mut buf).expect("a reply frame");
        buf.truncate(len);
        buf
    }

    #[test]
    fn answers_arp_for_gateway() {
        let net = SlirpNative::new(Vec::new(), 1500).unwrap();
        net.input(&arp_request());
        let reply = recv_frame(&net);

        let frame = Frame::from_slice(&reply);
        assert_eq!(frame.ether_type(), EtherType::ARP);
        assert_eq!(frame.dst_mac(), Some(MacAddr(GUEST_MAC)));
        assert_eq!(frame.src_mac(), Some(DEVICE_MAC));
        assert_eq!(u16::from_be_bytes([reply[20], reply[21]]), 2);
        assert_eq!(&reply[28..32], &GATEWAY.octets());
        assert_eq!(&reply[38..42], &GUEST_IP.octets());
        net.shutdown();
    }

    #[test]
    fn answers_dhcp_discover() {
        let net = SlirpNative::new(vec![Ipv4Addr::new(9, 9, 9, 9)], 1500).unwrap();
        net.input(&dhcp_discover());
        let reply = recv_frame(&net);

        let frame = Frame::from_slice(&reply);
        assert_eq!(frame.ether_type(), EtherType::IPV4);
        let packet = frame.payload();
        assert_eq!(packet[9], 17);
        assert_eq!(&packet[12..16], &GATEWAY.octets());
        assert_eq!(&packet[16..20], &[255u8; 4]);
        assert_eq!(u16::from_be_bytes([packet[20], packet[21]]), 67);
        assert_eq!(u16::from_be_bytes([packet[22], packet[23]]), 68);
        assert_eq!(packet[20 + 8 + 16..20 + 8 + 20], GUEST_IP.octets());
        assert!(reply.windows(4).any(|w| w == [9, 9, 9, 9]));
        net.shutdown();
    }
}
