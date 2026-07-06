"""Shared Nord theme snippets for generated static report pages."""

REPORT_THEME_HEAD_SCRIPT = """  <script>
    (function () {
      try {
        const saved = localStorage.getItem('yoolang-theme');
        const theme = saved === 'dark' || saved === 'light' ? saved : 'dark';
        document.documentElement.dataset.theme = theme;
        document.documentElement.style.colorScheme = theme;
      } catch (_) {
        document.documentElement.dataset.theme = 'dark';
      }
    })();
  </script>"""

REPORT_THEME_CSS = """
    :root,
    html[data-theme="light"] {
      color-scheme: light;
      --bg: #e5e9f0;
      --panel: #eceff4;
      --surface: #ffffff;
      --control: #f8fafc;
      --card: #f8fafc;
      --row-hover: #edf4fb;
      --text: #2e3440;
      --muted: #4c566a;
      --line: #d8dee9;
      --head: #d8dee9;
      --accent: #5e81ac;
      --accent-soft: #d8e8f0;
      --accent-line: #88c0d0;
      --measure-text: #3b4252;
      --good: #2f7d43;
      --good-soft: #dfeee4;
      --bad: #bf616a;
      --bad-soft: #f2dfe3;
      --warn: #a6652b;
      --warn-soft: #f2e7cf;
      --shadow: rgba(46, 52, 64, 0.12);
      --theme-toggle-bg: #eceff4;
      --theme-toggle-hover: #d8dee9;
      --theme-toggle-icon: #d08770;
    }
    html[data-theme="dark"] {
      color-scheme: dark;
      --bg: #2e3440;
      --panel: #3b4252;
      --surface: #434c5e;
      --control: #434c5e;
      --card: #434c5e;
      --row-hover: #3f4858;
      --text: #eceff4;
      --muted: #d8dee9;
      --line: #4c566a;
      --head: #434c5e;
      --accent: #88c0d0;
      --accent-soft: rgba(136, 192, 208, 0.16);
      --accent-line: #5e81ac;
      --measure-text: #e5e9f0;
      --good: #a3be8c;
      --good-soft: rgba(163, 190, 140, 0.18);
      --bad: #bf616a;
      --bad-soft: rgba(191, 97, 106, 0.18);
      --warn: #ebcb8b;
      --warn-soft: rgba(235, 203, 139, 0.18);
      --shadow: rgba(0, 0, 0, 0.28);
      --theme-toggle-bg: #434c5e;
      --theme-toggle-hover: #4c566a;
      --theme-toggle-icon: #ebcb8b;
    }
    .header-row {
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: 16px;
    }
    .header-row h1 {
      min-width: 0;
    }
    .theme-toggle {
      position: relative;
      display: inline-grid;
      place-items: center;
      flex: 0 0 auto;
      width: 42px;
      min-width: 42px;
      height: 42px;
      min-height: 42px;
      padding: 0;
      overflow: hidden;
      border: 1px solid var(--line);
      border-radius: 50%;
      background: var(--theme-toggle-bg);
      color: var(--theme-toggle-icon);
      box-shadow: 0 8px 22px var(--shadow);
    }
    .theme-toggle:hover {
      background: var(--theme-toggle-hover);
    }
    .theme-toggle:focus {
      border-color: var(--accent);
      box-shadow: 0 0 0 3px var(--accent-soft), 0 8px 22px var(--shadow);
      outline: none;
    }
    .theme-toggle svg {
      display: block;
      width: 20px;
      height: 20px;
      fill: none;
      stroke: currentColor;
      stroke-linecap: round;
      stroke-linejoin: round;
      stroke-width: 2;
    }
    .theme-icon {
      position: absolute;
      transition: opacity 160ms ease, transform 160ms ease;
    }
    .theme-icon.sun {
      opacity: 1;
      transform: translateY(0) rotate(0deg);
    }
    .theme-icon.moon {
      opacity: 0;
      transform: translateY(18px) rotate(-90deg);
    }
    html[data-theme="dark"] .theme-icon.sun {
      opacity: 0;
      transform: translateY(-18px) rotate(90deg);
    }
    html[data-theme="dark"] .theme-icon.moon {
      opacity: 1;
      transform: translateY(0) rotate(0deg);
    }
    .chip,
    .pill,
    button,
    .downloads a,
    .links a {
      background: var(--control);
    }
    .metric,
    .top-card {
      background: var(--card);
    }
    .measure {
      border-color: var(--accent-line);
      color: var(--measure-text);
    }
    th {
      background: var(--head);
    }
    tr:hover td {
      background: var(--row-hover);
    }
    a {
      color: var(--accent);
    }
"""

REPORT_THEME_TOGGLE_HTML = """<button class="theme-toggle" id="theme-toggle" type="button" aria-label="切换深色模式" title="切换深色模式">
      <span class="theme-icon sun" aria-hidden="true">
        <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"></circle><path d="M12 2v2"></path><path d="M12 20v2"></path><path d="m4.93 4.93 1.41 1.41"></path><path d="m17.66 17.66 1.41 1.41"></path><path d="M2 12h2"></path><path d="M20 12h2"></path><path d="m6.34 17.66-1.41 1.41"></path><path d="m19.07 4.93-1.41 1.41"></path></svg>
      </span>
      <span class="theme-icon moon" aria-hidden="true">
        <svg viewBox="0 0 24 24"><path d="M20.99 12.65A8.5 8.5 0 1 1 11.35 3.01 6.5 6.5 0 0 0 20.99 12.65Z"></path></svg>
      </span>
    </button>"""

REPORT_THEME_BIND_SCRIPT = """  <script>
    (function () {
      const toggle = document.getElementById('theme-toggle');
      const root = document.documentElement;
      if (!toggle) return;
      const labels = {
        light: '切换深色模式',
        dark: '切换浅色模式',
      };
      function applyTheme(theme, persist) {
        root.dataset.theme = theme;
        root.style.colorScheme = theme;
        toggle.setAttribute('aria-label', labels[theme]);
        toggle.setAttribute('title', labels[theme]);
        if (persist) {
          try {
            localStorage.setItem('yoolang-theme', theme);
          } catch (_) {}
        }
      }
      const current = root.dataset.theme === 'dark' ? 'dark' : 'light';
      applyTheme(current, false);
      toggle.addEventListener('click', function () {
        applyTheme(root.dataset.theme === 'dark' ? 'light' : 'dark', true);
      });
    })();
  </script>"""
