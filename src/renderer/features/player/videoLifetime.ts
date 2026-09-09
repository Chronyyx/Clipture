/** Release the browser's decoder, buffered media and outstanding source fetch. */
export function unloadVideo(video: HTMLVideoElement) {
  video.pause();
  video.removeAttribute('src');
  video.srcObject = null;
  video.load();
}
