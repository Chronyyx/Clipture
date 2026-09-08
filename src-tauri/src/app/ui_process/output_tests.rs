use super::*;
use std::{io, sync::mpsc, time::Duration};

struct PausedWriter {
    entered: Option<mpsc::Sender<()>>,
    resume: mpsc::Receiver<()>,
    finished: mpsc::Sender<Vec<u8>>,
    bytes: Vec<u8>,
}
impl Write for PausedWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if let Some(entered) = self.entered.take() {
            entered.send(()).unwrap();
            self.resume.recv().unwrap();
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
impl Drop for PausedWriter {
    fn drop(&mut self) {
        let _ = self.finished.send(std::mem::take(&mut self.bytes));
    }
}
fn reply(id: u64) -> Frame {
    Message::Reply {
        id,
        result: Ok(serde_json::Value::Null),
    }
    .into()
}

#[test]
fn saturated_reply_queue_keeps_latest_event_and_close_without_blocking_callbacks() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (resume_tx, resume_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let output = start(PausedWriter {
        entered: Some(entered_tx),
        resume: resume_rx,
        finished: done_tx,
        bytes: vec![],
    });
    output.send(reply(1)).unwrap();
    entered_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    output.try_send(reply(2)).unwrap();
    output.try_send(reply(3)).unwrap();
    assert!(output.try_send(reply(4)).is_err());
    for sequence in 0..100 {
        output
            .signal(
                Message::Event {
                    name: "library://changed".into(),
                    payload: serde_json::json!({"sequence": sequence}),
                }
                .into(),
            )
            .unwrap();
    }
    output.signal(Message::Close {}.into()).unwrap();
    output.signal(Message::Focus {}.into()).unwrap();
    assert_eq!(output.hints.lock().unwrap().events.len(), 1);
    assert!(output
        .signal(
            Message::Event {
                name: "unknown".into(),
                payload: serde_json::Value::Null
            }
            .into()
        )
        .is_err());
    resume_tx.send(()).unwrap();
    drop(output);
    let bytes = done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    let mut cursor = io::Cursor::new(bytes);
    let mut replies = vec![];
    let mut events = vec![];
    let mut closes = 0;
    while let Some(frame) = wire::read_frame(&mut cursor).unwrap() {
        match frame.message {
            Message::Reply { id, .. } => replies.push(id),
            Message::Event { payload, .. } => events.push(payload["sequence"].as_u64().unwrap()),
            Message::Close {} => closes += 1,
            unexpected => panic!("unexpected frame: {unexpected:?}"),
        }
    }
    assert_eq!(replies, vec![1, 2, 3]);
    assert_eq!(events, vec![99]);
    assert_eq!(closes, 1);
}

#[test]
fn writer_failure_disconnects_and_unblocks_waiting_producers() {
    struct Broken;
    impl Write for Broken {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::ErrorKind::BrokenPipe.into())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    let output = start(Broken);
    let (done_tx, done_rx) = mpsc::channel();
    thread::spawn(move || {
        for id in 1..=4 {
            if output.send(reply(id)).is_err() {
                done_tx.send(()).unwrap();
                return;
            }
        }
        panic!("broken writer accepted an unbounded queue");
    });
    done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
}
