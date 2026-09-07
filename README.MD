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

Successor to [HomeBrew Sorter GUI](https://www.gamebrew.org/wiki/HomeBrew_Sorter_GUI_PSP). Sort Categories & Apps on PSP (Go) & Adrenaline (Vita). Includes **alphabetize function(!)**, game title extraction, Game Categories (Lite) plugin in-app installation & configuration, ISO/CSO(v2)/ZSO/JSO/DAX support, and file operations.

---

### 1. Sort, Add, Delete, Rename, & Hide Categories in the XMB

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/9b42ea88-519d-451b-97d2-9976ed921f07" />

### 2. Delete, Rename, Hide in the XMB (not ISOs), & Sort/Alphabetize Apps by Filename/Title

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/941bd80d-e937-4f65-85ac-3e3cfefe1dc5" />

### 3. PSP Go and Adrenaline v8 Support (Memory Stick & System Storage)

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/5526f8af-d245-4325-a0dd-e456fceff37e" />

### 4. Copy/Move Apps Between Category Folders or Storage Devices

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/af5a143c-9a5d-4a3e-ae7e-610f31f3bd94" />

### 5. Icon & Title Support for All ARK CFW App Formats [PS1, Homebrew, ISO, CSO(v2), DAX, JSO, ZSO]

<img width="480" height="272" alt="Image" src="https://github.com/user-attachments/assets/7c9abe24-31b7-43b1-97c0-cede0b4b1e28" />

---

#### <ins>Installation</ins>:

0. Make sure you have a CFW installed on your PSP to run Homebrew apps like this one. [ARK-4](https://github.com/PSP-Archive/ARK-4) is the most modern PSP CFW.
    - This also works on Vita's Adrenaline PSP emulator (both [vanilla Adrenaline](https://github.com/isage/Adrenaline) and [ARK-4 Adrenaline](https://github.com/PSP-Archive/ARK-4#adrenaline-patched-pspemu)).
1. Extract `Homebrew.Sorter.Ultimate.zip` from the [latest Release](https://github.com/wad11656/Homebrew-Sorter-Ultimate/releases/tag/v1.26).
2. Copy the `/PSP/GAME/Homebrew Sorter Ultimate` folder to `[Memory Stick]:/PSP/GAME`.
3. Launch **Homebrew Sorter Ultimate** from your PSP's XMB/home screen.
4. If you want to file your PSP apps into categories (essentially folders) on your PSP's XMB/home screen, then enable the **Game Categories** plugin by selecting your CFW version under the `Game Categories:` section at the bottom of **Homebrew Sorter Ultimate**'s main screen.
    - PS Vita: If you're running vanilla (non-ARK) Adrenaline, select `PRO/ME` as your CFW. In order for the **Game Categories** plugin to start working, you may need to (temporarily) enable `Category prefix` from the `Game Categories Settings` screen after selecting a Storage Device. (Or you can enable the setting from `System Settings` in the XMB.)
5. In **Homebrew Sorter Ultimate**, select your Storage Device (`Memory Stick` or `System Storage`) to view your PSP apps.
6. When **Game Categories** is first enabled, your apps will be located in the `Uncategorized` category folder by default. Enable the `Uncategorized` category by going to `Game Categories Settings`, which appears at the top of the screen after selecting a Storage Device.

*Comprehensive video guide coming soon...*

#### <ins>Known Limitations</ins>:
- On PSP Go, you must run the app from Memory Stick to access Memory Stick storage.
- App file/folder names containing non-ASCII characters (e.g., `™`, `Æ`, `®`) cannot populate in the app. (Even though ISOs with non-ASCII characters in the filename can still successfully boot from the XMB.)
- On Adrenaline, in-app USB Mode is not supported.
- On vanilla (non-ARK) Adrenaline, the Game Categories plugin may require enabling the "Category prefix" plugin System Setting for it to start working.
- On vanilla (non-ARK) Adrenaline and LME CFW, the Game Categories plugin does not merge Category folders that share the same name across `/ISO` and `/PSP/GAME`.

#### <ins>Additional Info</ins>:
- Hold `R` on the app sorting list to reveal a `Grouped Alphabetize` function, allowing you to group your alphabetized apps by app type.
- You can disable the main screen's animations by removing the `/resources/animations` folder. (If you want different anitmations, you can add your own animation folders with animation `.png` frames named with the same frame-duration syntax as in the existing folders.)
- You can change the app background by swapping out `/resources/bkg.png`.
- A [personally-updated build of the Game Categories Lite plugin](https://github.com/wad11656/game-categories-lite) was created and is installed from within the app when you enable the **Game Categories** setting. This upgraded plugin hides Categories (new feature) & Apps on the XMB by defining their exact folder paths (including the `ef0`/`ms0` storage device on PSP Go)--instead of only hiding apps by listing their folder names.
  - Alternatively, you can still use a different Game Categories Lite plugin that you already had pre-installed, if you prefer. Just make sure it's installed on your PSP as `category_lite.prx`, and select the **Use my own existing category_lite.prx plugin** option in the **Game Categories** menu on the app's main screen.

<div>
<img width="536" height="272" alt="Image" src="https://github.com/user-attachments/assets/9a8393b6-50af-4ae5-99d1-27b4aaab8260" />

#### <ins>Acknowledgements</ins>:
- [CMFileManager by joel16](https://github.com/joel16/CMFileManager-PSP) was used as the base for this app to help with file operations and compiling with the modern PSP SDK.
- Thank you [kathyrollo](https://github.com/kathyrollo) for the kind promo and thorough QA.

<div id="vibe-code-acknowledgement">
  &#8194;
  <img
    width="20"
    alt="Image"
    src="https://github.com/user-attachments/assets/a2474b23-8cff-4ee9-96e3-b7c9d94ee3a4"
    style="vertical-align: middle;"
  />
  &#8202;
  This app was agentically coded as a hobby project by someone with lots of pre-LLM-era coding experience.
</div>
