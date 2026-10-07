package com.wither.nsmbw;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/** Start screen: is the game data there, play, or import it from an extracted disc folder. */
public final class LauncherActivity extends Activity {
    private static final int REQUEST_IMPORT = 1;
    /** Set by GameActivity.restartForSession: open the game again once its old process is gone. */
    static final String EXTRA_RESTART_GAME = "restartGame";

    private TextView status;
    private Button play;
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final int pad = dp(24);
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, pad, pad, pad);
        column.setGravity(Gravity.CENTER_HORIZONTAL);

        final TextView title = text("NSMBW", 34);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);
        column.addView(text("New Super Mario Bros. Wii, recompiled to run natively", 15));

        status = text("", 16);
        status.setPadding(0, dp(24), 0, dp(16));
        column.addView(status);

        play = button("Play");
        play.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        column.addView(play);

        final Button importButton = button("Import Game Folder…");
        importButton.setOnClickListener(v -> pickFolder());
        column.addView(importButton);

        final TextView help = text(
                "Use your own extracted copy of the game (SMNE01): the folder that holds files/ and "
                + "sys/, or Dolphin's DATA folder. Importing copies it into this app.\n\n"
                + "Or copy that folder over USB to:\nAndroid/data/" + getPackageName() + "/files/game", 13);
        help.setPadding(0, dp(24), 0, 0);
        help.setTextColor(Color.rgb(170, 178, 186));
        column.addView(help);

        final ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        setContentView(scroll);
        restartGameIfAsked(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        restartGameIfAsked(intent);
    }

    /** Network play: the game asked to be started again into a LAN session. */
    private void restartGameIfAsked(Intent intent) {
        if (intent == null || !intent.getBooleanExtra(EXTRA_RESTART_GAME, false)) {
            return;
        }
        intent.removeExtra(EXTRA_RESTART_GAME);
        status.setText("Starting the LAN game...");
        main.postDelayed(() -> startActivity(new Intent(this, GameActivity.class)), 700);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        final boolean present = AppFiles.hasGameData(this);
        status.setText(present ? "Game data found." : "No game data yet. Import your game folder to start.");
        play.setEnabled(present);
    }

    private void pickFolder() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(intent, REQUEST_IMPORT);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_IMPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            importFrom(data.getData());
        }
    }

    // --- import --------------------------------------------------------------------------------

    private static final class Entry {
        final String id, name;
        final boolean directory;
        Entry(String id, String name, boolean directory) { this.id = id; this.name = name; this.directory = directory; }
    }

    private void importFrom(Uri tree) {
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        final TextView label = text("Looking at the folder…", 14);
        final LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(16), dp(24), dp(8));
        box.addView(bar);
        box.addView(label);
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Importing game data").setView(box).setCancelable(false).show();

        new Thread(() -> {
            String result;
            try {
                result = copyGame(tree, (done, total) -> main.post(() -> {
                    bar.setMax(total);
                    bar.setProgress(done);
                    label.setText(done + " of " + total + " files");
                }));
            } catch (Exception e) {
                result = "Import failed: " + e.getMessage();
            }
            final String message = result;
            main.post(() -> {
                dialog.dismiss();
                refresh();
                new AlertDialog.Builder(this).setMessage(message).setPositiveButton("OK", null).show();
            });
        }, "game-import").start();
    }

    private interface Progress { void update(int done, int total); }

    /** Copies into game.importing, checks it, then swaps it in for game. */
    private String copyGame(Uri tree, Progress progress) throws IOException {
        final ContentResolver resolver = getContentResolver();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        if (!looksLikeGame(resolver, tree, rootId)) {
            String nested = null;
            for (Entry e : list(resolver, tree, rootId)) {
                if (e.directory && e.name.equalsIgnoreCase("DATA") && looksLikeGame(resolver, tree, e.id)) nested = e.id;
            }
            if (nested == null) {
                return "That folder is not an extracted game: it needs files/ and sys/ (or a DATA folder holding them).";
            }
            rootId = nested;
        }
        final List<String[]> files = new ArrayList<>();  // {documentId, relative path}
        collect(resolver, tree, rootId, "", files);

        final File staging = new File(AppFiles.dataDir(this), "game.importing");
        AppFiles.deleteRecursively(staging);
        for (int i = 0; i < files.size(); ++i) {
            final File target = new File(staging, files.get(i)[1]);
            final File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            final Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, files.get(i)[0]);
            try (InputStream in = resolver.openInputStream(document); OutputStream out = new FileOutputStream(target)) {
                if (in == null) throw new IOException("cannot read " + files.get(i)[1]);
                AppFiles.copy(in, out);
            }
            progress.update(i + 1, files.size());
        }
        if (!AppFiles.isGameRoot(staging)) {
            AppFiles.deleteRecursively(staging);
            return "The copied folder has no sys/main.dol; nothing was changed.";
        }
        final File game = AppFiles.gameDir(this);
        AppFiles.deleteRecursively(game);
        if (!staging.renameTo(game)) throw new IOException("cannot move the imported data into place");
        return "Imported " + files.size() + " files. Tap Play to start.";
    }

    private boolean looksLikeGame(ContentResolver resolver, Uri tree, String id) {
        boolean sys = false, filesDir = false;
        for (Entry e : list(resolver, tree, id)) {
            if (e.directory && e.name.equals("sys")) sys = true;
            if (e.directory && e.name.equals("files")) filesDir = true;
        }
        return sys && filesDir;
    }

    private void collect(ContentResolver resolver, Uri tree, String id, String prefix, List<String[]> out) {
        for (Entry e : list(resolver, tree, id)) {
            final String path = prefix.isEmpty() ? e.name : prefix + "/" + e.name;
            if (e.directory) collect(resolver, tree, e.id, path, out);
            else out.add(new String[] {e.id, path});
        }
    }

    private List<Entry> list(ContentResolver resolver, Uri tree, String id) {
        final List<Entry> entries = new ArrayList<>();
        final Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        final String[] columns = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        };
        try (Cursor c = resolver.query(children, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                entries.add(new Entry(c.getString(0), c.getString(1),
                        DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))));
            }
        }
        return entries;
    }

    // --- views ---------------------------------------------------------------------------------

    private TextView text(String value, int sp) {
        final TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setTextColor(Color.WHITE);
        view.setGravity(Gravity.CENTER_HORIZONTAL);
        return view;
    }

    private Button button(String label) {
        final Button view = new Button(this);
        view.setText(label);
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(280), LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(8);
        view.setLayoutParams(params);
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
