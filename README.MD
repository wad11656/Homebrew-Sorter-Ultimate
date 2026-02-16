# Homebrew Sorter Ultimate
<div align="right">
  <a href="#vibe-code-acknowledgement">
    <img width="25" alt="Image" src="https://github.com/user-attachments/assets/a2474b23-8cff-4ee9-96e3-b7c9d94ee3a4" />
  </a>
</div>
<div align="center">
<img width="650" alt="Image" src="https://github.com/user-attachments/assets/519513a8-6007-4118-acab-faf7247b94dc" />
</div>
<br/>

Successor to [HomeBrew Sorter GUI](https://www.gamebrew.org/wiki/HomeBrew_Sorter_GUI_PSP). Sort Categories & Apps on PSP (Go) & Adrenaline (Vita). Includes alphabetize function, full Game Categories (Lite) plugin integration, and file operations.

---

### 1. Sort, Add, Delete, Rename, & Hide Categories in the XMB

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/9b42ea88-519d-451b-97d2-9976ed921f07" />

### 2. Delete, Rename, Hide in the XMB (not ISOs), & Sort/Alphabetize Apps by Filename/Title

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/941bd80d-e937-4f65-85ac-3e3cfefe1dc5" />

### 3. PSP Go Support (Memory Stick & Internal Storage)

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/5526f8af-d245-4325-a0dd-e456fceff37e" />

### 4. Copy/Move Apps Between Category Folders or Storage Devices

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/af5a143c-9a5d-4a3e-ae7e-610f31f3bd94" />

### 5. Icon & Title Support for All ARK CFW App Formats [PS1, Homebrew, ISO, CSO(v2), DAX, JSO, ZSO]

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/7c9abe24-31b7-43b1-97c0-cede0b4b1e28" />

---

<ins>Known Limitations</ins>:
- On PSP Go, you must run the app from Memory Stick to access Memory Stick storage.
- On Adrenaline, in-app USB Mode is not supported.
- On vanilla (non-ARK) Adrenaline, the integrated Game Categories plugin requires the "Category prefix" plugin setting to be enabled.
- On vanilla (non-ARK) Adrenaline and LME CFW, the Game Categories plugin does not merge Category folders that share the same name across `/ISO` and `/PSP/GAME`.

<ins>Additional Info</ins>:
- You can bulk-select apps by holding square and pressing up or down. (This will select every unchecked app in that direction until it hits the end of the app list <ins>or</ins> another checked app.)
- You can disable the main screen's animations by removing all the folders in the `/resources/animations` folder. (Or even add your own animation folders with animation `.png` frames named the same way as in the existing folders.)
- You can change the app background by swapping out `/resources/bkg.png`.
- A [personally-updated build of the Game Categories Lite plugin](https://github.com/wad11656/game-categories-lite) was created and integrated with this app to hide Categories & Apps on the XMB at a more granular level: by storage device/exact filepath.
  - You can use your existing legacy Game Categories Lite plugin instead if you prefer. Just make sure it's installed on your PSP as `category_lite.prx`, and select the **Use existing legacy category_lite.prx plugin** option in the **Game Categories** menu on the app's main screen.

<div>
<img width="536" height="272" alt="Image" src="https://github.com/user-attachments/assets/c8906bf6-5419-41a5-8989-92087f7cd5c6" />

<ins>Acknowledgements</ins>:

- [CMFileManager by joel16](https://github.com/joel16/CMFileManager-PSP) was used as the base for this app to help with file operations and compiling with the modern PSP SDK.
- Thank you [u/k_rollo](https://www.reddit.com/user/k_rollo/) for the kind promo and thorough QA.

<div id="vibe-code-acknowledgement">
  &#8194;
  <img
    width="20"
    alt="Image"
    src="https://github.com/user-attachments/assets/a2474b23-8cff-4ee9-96e3-b7c9d94ee3a4"
    style="vertical-align: middle;"
  />
  &#8202;
  This app was "vibe-coded" as a hobby project by someone with lots of pre-LLM-era coding experience.
</div>
