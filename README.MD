# Homebrew Sorter Ultimate
<div align="right">
  <a href="#vibe-code-acknowledgement">
    <img width="25" alt="Image" src="https://github.com/user-attachments/assets/a2474b23-8cff-4ee9-96e3-b7c9d94ee3a4" />
  </a>
</div>
<div align="center">
<img width="650" src="https://github.com/user-attachments/assets/b1ef3b48-57ac-45e3-aecc-64b13e5f4901" />
</div>
<br/>

Successor to [HomeBrew Sorter](https://github.com/FPSUsername/HomeBrew_Sorter). Sort Categories & Apps on PSP (Go) & Adrenaline (Vita). Includes alphabetize function, full Game Categories (Lite) plugin integration, and file operations.

---

### 1. Sort, Add, Delete, Rename, and Hide Categories in the XMB

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/9b42ea88-519d-451b-97d2-9976ed921f07" />

### 2. Delete, Rename and Sort/Alphabetize Apps by Filename or Title

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/941bd80d-e937-4f65-85ac-3e3cfefe1dc5" />

### 3. PSP Go Support (Memory Stick & Internal Storage)

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/5526f8af-d245-4325-a0dd-e456fceff37e" />

### 4. Copy/Move Apps Between Category Folders or Storage Devices

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/af5a143c-9a5d-4a3e-ae7e-610f31f3bd94" />

### 5. Icon & Title Extraction for All ARK-4-Supported App Types [PS1, Homebrew, ISO, CSO(v2), DAX, JSO, ZSO]

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/7c9abe24-31b7-43b1-97c0-cede0b4b1e28" />

---

<ins>Known Limitations</ins>:
- On PSP Go, you must run the app from Memory Stick to access Memory Stick storage.
- On Adrenaline, in-app USB Mode is not supported.
- On vanilla (non-ARK) Adrenaline, the integrated Game Categories plugin requires the "Category prefix" plugin setting to be enabled.
- On vanilla (non-ARK) Adrenaline and LME CFW, the Game Categories plugin does not merge Category folders that share the same name across `/ISO` and `/PSP/GAME`.

<ins>Additional Info</ins>:
- You can bulk select apps by holding square and pressing up or down. (This will select every unchecked app in that direction until it hits the end of the app list or a checked app.)
- You can disable the main screen's animations by removing all the folders in the `/resources/animations` folder. (Or even add your own animation folders with animation `.png` frames named the same way as in the existing folders.)
- You can change the app background by swapping out `/resources/bkg.png`.
- A [personally-updated build of the Game Categories Lite plugin](https://github.com/wad11656/game-categories-lite) was created and integrated with this app to restore the functionality of hiding Categories from the XMB.

<ins>Acknowledgements</ins>:

- [CMFileManager by joel16](https://github.com/joel16/CMFileManager-PSP) was used as the base for this app to help with file operations and compiling with the modern PSP SDK.

<div>
<img width="38" height="23" alt="Image" src="https://github.com/user-attachments/assets/745b7aa8-5125-44a3-a346-3970dc9f7b6e" />This app was "vibe-coded" as a hobby project by someone technically-minded with ample pre-LLM-era coding and debugging experience.</dd>
</div>
