// ConfirmDialog.tsx
// Extracted from App.tsx (was two identical inline blocks -- one in the pre-login
// screen, one in the main app screen). Pure presentational; App.tsx still owns the
// `confirmDialog` state and the `confirmThenRun` helper that populates it.

export interface ConfirmDialogState { title: string; message: string; onConfirm: () => void; }

interface Props {
  dialog: ConfirmDialogState | null;
  onCancel: () => void;
}

export default function ConfirmDialog({ dialog, onCancel }: Props) {
  if (!dialog) return null;
  return (
    <div className="dlg-overlay" onClick={onCancel}>
      <div className="dlg-box" onClick={e => e.stopPropagation()}>
        <div className="dlg-header">
          <div>
            <div className="dlg-title">{dialog.title}</div>
            <div className="dlg-subtitle">{dialog.message}</div>
          </div>
        </div>
        <div className="dlg-actions">
          <button type="button" className="dlg-cancel" onClick={onCancel}>Cancel</button>
          <button type="button" className="dlg-danger" onClick={() => { const fn = dialog.onConfirm; onCancel(); fn(); }}>Delete</button>
        </div>
      </div>
    </div>
  );
}
