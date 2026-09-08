use super::*;
use std::io::Cursor;

#[test]
fn control_and_binary_frames_round_trip_without_base64() {
    let mut bytes = vec![];
    write_frame(
        &mut bytes,
        &Message::Ready {
            version: PROTOCOL_VERSION,
        }
        .into(),
    )
    .unwrap();
    let body = vec![0, 255, 13, 10, 0];
    write_frame(
        &mut bytes,
        &Frame {
            message: Message::MediaReply {
                id: 7,
                status: 206,
                headers: vec![],
            },
            binary: body.clone(),
        },
    )
    .unwrap();
    let mut reader = Cursor::new(bytes);
    assert!(matches!(
        read_frame(&mut reader).unwrap().unwrap().message,
        Message::Ready { version: 1 }
    ));
    assert_eq!(read_frame(&mut reader).unwrap().unwrap().binary, body);
    assert!(read_frame(&mut reader).unwrap().is_none());
}

#[test]
fn oversized_and_truncated_frames_fail_closed() {
    let mut oversized = (MAXIMUM_JSON_BYTES as u32 + 1).to_le_bytes().to_vec();
    oversized.extend(0u32.to_le_bytes());
    assert_eq!(
        read_frame(&mut Cursor::new(oversized)).unwrap_err().kind(),
        io::ErrorKind::InvalidData
    );
    let mut valid = vec![];
    write_frame(&mut valid, &Message::Focus {}.into()).unwrap();
    for end in 1..valid.len() {
        assert!(
            read_frame(&mut Cursor::new(&valid[..end])).is_err(),
            "truncated at {end}"
        );
    }
    let mut truncated_binary = vec![];
    write_frame(
        &mut truncated_binary,
        &Frame {
            message: Message::MediaReply {
                id: 1,
                status: 200,
                headers: vec![],
            },
            binary: vec![1, 2],
        },
    )
    .unwrap();
    truncated_binary.pop();
    assert!(read_frame(&mut Cursor::new(truncated_binary)).is_err());
}

#[test]
fn non_media_binary_and_unknown_message_fields_are_rejected() {
    assert!(write_frame(
        &mut vec![],
        &Frame {
            message: Message::Focus {},
            binary: vec![1]
        }
    )
    .is_err());
    let json = br#"{"type":"focus","untrusted":"extra"}"#;
    let mut frame = (json.len() as u32).to_le_bytes().to_vec();
    frame.extend(0u32.to_le_bytes());
    frame.extend(json);
    assert!(read_frame(&mut Cursor::new(frame)).is_err());
}
