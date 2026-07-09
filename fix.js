const fs = require('fs');
const file = "c:/Users/aidav/OneDrive/Desktop/bot/2/Clipture/src/renderer/styles.css";
let content = fs.readFileSync(file, 'utf-8');

// I need to replace from .play-badge all the way down to the duplicate clip-name:hover
// So I will find the first index of .play-badge, and replace everything up to the real .clip-card-footer
const startIndex = content.indexOf('.play-badge {');
const endIndex = content.indexOf('.clip-card-footer {');

const correctBlock = `.play-badge {
  left: 50%;
  top: 50%;
  width: 44px;
  height: 44px;
  display: grid;
  place-items: center;
  border-radius: 999px;
  color: #171717;
  background: rgba(226, 232, 240, 0.94);
  opacity: 0;
  transform: translate(-50%, -50%) scale(0.92);
  transition: opacity 140ms ease, transform 140ms ease;
}

.thumbnail-button:hover .play-badge,
.clip-card.active .play-badge {
  opacity: 1;
  transform: translate(-50%, -50%) scale(1);
}

.clip-info {
  display: grid;
  gap: 5px;
  padding-top: 9px;
  min-width: 0;
}

.clip-info span {
  color: #a5adba;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.clip-name {
  border: 0;
  padding: 0;
  color: #f4f4f5;
  background: transparent;
  font-weight: 800;
  text-align: left;
  cursor: pointer;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.clip-name:hover {
  color: #e2e8f0;
}

`;

content = content.substring(0, startIndex) + correctBlock + content.substring(endIndex);
fs.writeFileSync(file, content);
console.log("Fixed styles.css");
