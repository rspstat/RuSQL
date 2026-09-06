// lib/measureText.ts
// Text-width measurement (한글/CJK 포함, canvas 사용), moved out of App.tsx unchanged
// during the App.tsx UI-section extraction refactor -- no behavior change.
// Shared by the results table (App.tsx) and ErdView's data panel.

let _measureCtx: CanvasRenderingContext2D | null = null;
export const measureTextPx = (text: string): number => {
  if (!_measureCtx) {
    const c = document.createElement('canvas');
    _measureCtx = c.getContext('2d');
    if (!_measureCtx) return text.length * 8;
    _measureCtx.font = '13px Consolas, "Malgun Gothic", monospace';
  }
  return _measureCtx.measureText(text).width;
};
