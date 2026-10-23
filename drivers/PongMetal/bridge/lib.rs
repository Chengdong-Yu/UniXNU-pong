// SPDX-License-Identifier: MIT
//! Pong's Metal object C ABI over metal2vulkan/Turnip.
//! Builds product objects only; no embedded diagnostics, shader fixtures,
//! host simulator, auto-publication or fabricated family capabilities.
#[path = "NativeMetalObjects.rs"]
pub mod objects;

/// Select the AIR64 member of Apple's universal metallib containers. Thin
/// metallibs keep their original bytes; GPU machine code is never substituted
/// for AIR. Offsets and the complete architecture table must fit the input.
pub fn air_slice(bytes: &[u8]) -> Option<&[u8]> {
    let word = |at: usize| {
        let end = at.checked_add(4)?;
        let value: [u8; 4] = bytes.get(at..end)?.try_into().ok()?;
        Some(u32::from_be_bytes(value))
    };
    if word(0)? != 0xcafe_babe {
        return Some(bytes);
    }
    let count = word(4)? as usize;
    let table_end = 8usize.checked_add(count.checked_mul(20)?)?;
    bytes.get(..table_end)?;
    for i in 0..count {
        let at = 8 + 20 * i;
        if word(at)? == 0x0100_0017 {
            let offset = word(at + 8)? as usize;
            let size = word(at + 12)? as usize;
            if offset < table_end || size == 0 {
                return None;
            }
            return bytes.get(offset..offset.checked_add(size)?);
        }
    }
    None
}
