import { Check, ExternalLink, Feather, Leaf, Moon, Palette, RefreshCw, Sun } from 'lucide-react';
import { useEffect, useState } from 'react';
import type { CSSProperties } from 'react';
import type { ClipSettings, ThemeFontId } from '../../../../shared/types';
import { clipture } from '../../../platform';
import { applyUiTheme, refreshLocalThemeFont } from '../../../theme';
import { ThemeColorField } from './ThemeColorField';

interface CustomizeSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

const themes = [
  { id: 'graphite', label: 'Graphite', Icon: Moon },
  { id: 'light', label: 'Light', Icon: Sun },
  { id: 'glitten', label: 'Glitten', Icon: Feather },
  { id: 'milate', label: 'Milate', Icon: Leaf },
  { id: 'custom', label: 'Custom', Icon: Palette }
] as const;

export function CustomizeSettings({ settings, onChange }: CustomizeSettingsProps) {
  const [mainColor, setMainColor] = useState(settings.customMainColor);
  const [accentColor, setAccentColor] = useState(settings.customAccentColor);
  const [themeFontAvailable, setThemeFontAvailable] = useState<boolean>();
  const selectedThemeFont: ThemeFontId | undefined = settings.uiTheme === 'glitten' || settings.uiTheme === 'milate'
    ? settings.uiTheme
    : undefined;
  const selectedThemeFontLabel = selectedThemeFont === 'glitten' ? 'Glitten' : 'Milate';

  useEffect(() => setMainColor(settings.customMainColor), [settings.customMainColor]);
  useEffect(() => setAccentColor(settings.customAccentColor), [settings.customAccentColor]);

  useEffect(() => {
    if (!selectedThemeFont) {
      setThemeFontAvailable(undefined);
      return;
    }
    let active = true;
    const refresh = () => {
      setThemeFontAvailable(undefined);
      void refreshLocalThemeFont(selectedThemeFont).then((available) => {
        if (active) setThemeFontAvailable(available);
      });
    };
    refresh();
    window.addEventListener('focus', refresh);
    return () => {
      active = false;
      window.removeEventListener('focus', refresh);
    };
  }, [selectedThemeFont]);

  function previewCustom(nextMain: string, nextAccent: string) {
    setMainColor(nextMain);
    setAccentColor(nextAccent);
    applyUiTheme({ uiTheme: 'custom', customMainColor: nextMain, customAccentColor: nextAccent });
  }

  const customPreviewStyle = {
    '--preview-main': mainColor,
    '--preview-accent': accentColor
  } as CSSProperties;

  return (
    <div className='settings-group single-column customize-settings-group'>
      <div className='customize-settings-panel'>
        <div className='audio-settings-heading'><h2>Appearance</h2></div>
        <div className='theme-options' role='radiogroup' aria-label='Interface theme'>
          {themes.map(({ id, label, Icon }) => (
            <button
              aria-checked={settings.uiTheme === id}
              className={settings.uiTheme === id ? 'theme-option selected' : 'theme-option'}
              key={id}
              onClick={() => onChange({ uiTheme: id })}
              role='radio'
              type='button'
            >
              <span className={'theme-preview ' + id} style={id === 'custom' ? customPreviewStyle : undefined} aria-hidden='true'>
                <span className='theme-preview-sidebar' />
                <span className='theme-preview-content'>
                  <span className='theme-preview-line' />
                  <span className='theme-preview-button' />
                </span>
              </span>
              <span className='theme-option-label'><Icon size={17} /> {label}</span>
              {settings.uiTheme === id && <Check className='theme-option-check' size={17} aria-hidden='true' />}
            </button>
          ))}
        </div>

        {selectedThemeFont && (
          <div className='theme-font-tools'>
            <div className='theme-font-status'>
              <strong>{selectedThemeFontLabel} typeface</strong>
              <span>{themeFontAvailable === undefined ? 'Checking...' : themeFontAvailable ? 'Installed' : 'Fallback active'}</span>
            </div>
            <div className='theme-font-actions'>
              <button className='secondary-button' type='button' onClick={() => void clipture.openThemeFontDownload(selectedThemeFont)}>
                <ExternalLink size={16} /> Get {selectedThemeFontLabel}
              </button>
              <button
                className='secondary-button'
                type='button'
                onClick={() => {
                  setThemeFontAvailable(undefined);
                  void refreshLocalThemeFont(selectedThemeFont).then(setThemeFontAvailable);
                }}
              >
                <RefreshCw size={16} /> Refresh font
              </button>
            </div>
          </div>
        )}

        {settings.uiTheme === 'custom' && (
          <div className='custom-colors'>
            <h3>Custom colors</h3>
            <div className='theme-color-grid'>
              <ThemeColorField
                label='Main color'
                value={settings.customMainColor}
                onPreview={(color) => previewCustom(color, accentColor)}
                onCommit={(customMainColor) => onChange({ customMainColor })}
              />
              <ThemeColorField
                label='Accent color'
                value={settings.customAccentColor}
                onPreview={(color) => previewCustom(mainColor, color)}
                onCommit={(customAccentColor) => onChange({ customAccentColor })}
              />
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
