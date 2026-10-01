// Invite handoff (ADR 0011). Reads the friend code from the URL fragment,
// which browsers never send to the server, and builds the app link.
(function () {
  // z-base-32 encoding of a 32-byte public key: exactly 52 characters.
  var alphabet = /^[ybndrfg8ejkmcpqxot1uwisza345h769]{52}$/;
  var params = new URLSearchParams(location.hash.slice(1));
  var code = (params.get("c") || "").trim();
  var name = (params.get("n") || "").replace(/[\u0000-\u001f\u007f]/g, "").trim().slice(0, 40);
  if (!alphabet.test(code)) {
    document.getElementById("broken").hidden = false;
    return;
  }
  var app = "clipture://add/" + code + (name ? "?name=" + encodeURIComponent(name) : "");
  document.getElementById("invite").hidden = false;
  // textContent only: the name comes from whoever made the link.
  document.getElementById("initial").textContent = (name.charAt(0) || "?").toUpperCase();
  if (name) document.getElementById("title").textContent = name + " invited you to be friends on Clipture";
  var open = document.getElementById("open");
  open.href = app;
  var copy = document.getElementById("copy");
  copy.addEventListener("click", function () {
    navigator.clipboard.writeText(app).then(function () {
      copy.textContent = "Copied. Paste it in Clipture.";
    }, function () {
      copy.textContent = "Couldn't copy. Use Open in Clipture instead.";
    });
  });
})();
