import { useState } from "react";
import { useClipThumbnail } from "./useClipThumbnail";

/** A clip's thumbnail over a skeleton: the skeleton stays until the image has
 * decoded, then the image fades in, so a card never shows an empty box while
 * its picture loads. Fills its (positioned) parent. */
export function ClipThumbnail({
  filePath,
  enabled = true,
  eager = false
}: {
  filePath: string;
  /** Off for cards far outside the view. */
  enabled?: boolean;
  /** The clip in the player: load before the browser decides it is visible. */
  eager?: boolean;
}) {
  const { url, failed } = useClipThumbnail(filePath, enabled);
  const [shown, setShown] = useState("");
  const loaded = Boolean(url) && shown === url;

  return (
    <>
      {!loaded && <span className="thumbnail-skeleton" aria-hidden="true" />}
      {url && (
        <img
          className={loaded ? "clip-thumbnail loaded" : "clip-thumbnail"}
          src={url}
          alt=""
          loading={eager ? "eager" : "lazy"}
          decoding="async"
          onLoad={() => setShown(url)}
          onError={failed}
        />
      )}
    </>
  );
}
