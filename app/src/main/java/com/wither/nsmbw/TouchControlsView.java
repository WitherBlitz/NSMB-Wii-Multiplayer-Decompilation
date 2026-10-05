package com.wither.nsmbw;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.SparseIntArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

/**
 * The on-screen Wii Remote, laid out the way the remote is held sideways: the cross-pad under the
 * left thumb, 1 and 2 under the right one. Every finger is tracked on its own; a finger that starts
 * on the pad steers it until lifted, a finger on the buttons may slide from one to another.
 */
final class TouchControlsView extends View {
    /** Bits as the native side expects them (VirtualRemote::TouchButton), plus local extras. */
    static final int LEFT = 0x001, RIGHT = 0x002, UP = 0x004, DOWN = 0x008;
    static final int ONE = 0x010, TWO = 0x020, A = 0x040, PLUS = 0x080, MINUS = 0x100;
    private static final int SHAKE = 0x1000, MENU = 0x2000;
    private static final int PAD = 0x4000;  // pointer captured by the cross-pad

    interface Listener {
        void onTouchInput(int buttons, boolean shake);
        void onMenu();
    }

    private static final class Button {
        final int bit;
        final String label;
        float x, y, r;
        Button(int bit, String label) { this.bit = bit; this.label = label; }
        boolean hit(float px, float py) {
            final float dx = px - x, dy = py - y, reach = r * 1.25f;
            return dx * dx + dy * dy <= reach * reach;
        }
    }

    private final Button[] buttons = {
        new Button(TWO, "2"), new Button(ONE, "1"), new Button(A, "A"), new Button(SHAKE, "SHAKE"),
        new Button(MINUS, "−"), new Button(PLUS, "+"), new Button(MENU, "⋯"),
    };
    private float padX, padY, padR;
    private final SparseIntArray pointers = new SparseIntArray();  // pointer id -> bits it holds
    private int held;
    private Listener listener;
    private float opacity = 0.6f;
    private boolean controlsVisible = true;
    private boolean haptics = true;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arrow = new Path();
    private final RectF rect = new RectF();

    TouchControlsView(Context context) {
        super(context);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setFocusable(false);
    }

    void setListener(Listener listener) { this.listener = listener; }

    void setOpacity(float opacity) { this.opacity = opacity; invalidate(); }

    void setHaptics(boolean haptics) { this.haptics = haptics; }

    /** Hidden controls still leave the menu button, so the settings stay reachable. */
    void setControlsVisible(boolean visible) {
        controlsVisible = visible;
        release();
        invalidate();
    }

    /** Lets go of everything, e.g. when the app loses focus or a menu opens. */
    void release() {
        pointers.clear();
        update(0);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls(w, h);
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        layoutControls(getWidth(), getHeight());
        return super.onApplyWindowInsets(insets);
    }

    private void layoutControls(int w, int h) {
        if (w == 0 || h == 0) return;
        // Keep the thumbs' controls clear of a camera cutout on either short edge.
        float left = 0, right = 0;
        final WindowInsets insets = getRootWindowInsets();
        if (insets != null && insets.getDisplayCutout() != null) {
            left = insets.getDisplayCutout().getSafeInsetLeft();
            right = insets.getDisplayCutout().getSafeInsetRight();
        }
        final float u = h;  // everything scales with the short side
        padR = 0.21f * u;
        padX = left + 0.07f * u + padR;
        padY = 0.66f * u;
        place(TWO, w - right - 0.17f * u, 0.74f * u, 0.11f * u);
        place(ONE, w - right - 0.43f * u, 0.80f * u, 0.11f * u);
        place(A, w - right - 0.43f * u, 0.52f * u, 0.075f * u);
        place(SHAKE, w - right - 0.17f * u, 0.44f * u, 0.085f * u);
        place(MINUS, w / 2f - 0.13f * u, 0.08f * u, 0.05f * u);
        place(PLUS, w / 2f + 0.13f * u, 0.08f * u, 0.05f * u);
        place(MENU, w - right - 0.08f * u, 0.08f * u, 0.05f * u);
        invalidate();
    }

    private void place(int bit, float x, float y, float r) {
        for (Button b : buttons) {
            if (b.bit == bit) { b.x = x; b.y = y; b.r = r; }
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            release();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int i = e.getActionIndex();
            pointers.put(e.getPointerId(i), press(e.getX(i), e.getY(i)));
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); ++i) {
                final int id = e.getPointerId(i);
                final int current = pointers.get(id, 0);
                if ((current & PAD) != 0) {
                    pointers.put(id, PAD | padDirection(e.getX(i), e.getY(i)));
                } else if (current != 0 && (current & MENU) == 0) {
                    final int slid = buttonAt(e.getX(i), e.getY(i));
                    pointers.put(id, slid != MENU ? slid : 0);
                }
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            final int i = e.getActionIndex();
            final int id = e.getPointerId(i);
            final int bits = pointers.get(id, 0);
            pointers.delete(id);
            if ((bits & MENU) != 0 && buttonAt(e.getX(i), e.getY(i)) == MENU && listener != null) {
                listener.onMenu();
            }
        }
        int combined = 0;
        for (int i = 0; i < pointers.size(); ++i) {
            combined |= pointers.valueAt(i);
        }
        update(combined & ~PAD);
        return true;
    }

    private int press(float x, float y) {
        if (controlsVisible) {
            final float dx = x - padX, dy = y - padY;
            if (dx * dx + dy * dy <= padR * padR * 1.6f) {
                return PAD | padDirection(x, y);
            }
        }
        return buttonAt(x, y);
    }

    private int buttonAt(float x, float y) {
        for (Button b : buttons) {
            if ((controlsVisible || b.bit == MENU) && b.hit(x, y)) return b.bit;
        }
        return 0;
    }

    /** Eight-way: a direction counts within 67.5 degrees of its axis, so diagonals press two. */
    private int padDirection(float x, float y) {
        final float dx = x - padX, dy = y - padY;
        if (dx * dx + dy * dy < (0.22f * padR) * (0.22f * padR)) return 0;
        int bits = 0;
        final float slope = 0.4142136f;  // tan(22.5 degrees)
        if (Math.abs(dx) >= slope * Math.abs(dy)) bits |= dx < 0 ? LEFT : RIGHT;
        if (Math.abs(dy) >= slope * Math.abs(dx)) bits |= dy < 0 ? UP : DOWN;
        return bits;
    }

    private void update(int bits) {
        if (bits == held) return;
        final int pressed = bits & ~held & ~MENU;
        held = bits;
        if (pressed != 0 && haptics) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        if (listener != null) {
            listener.onTouchInput(bits & 0x1FF, (bits & SHAKE) != 0);
        }
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final int base = Math.round(255 * opacity);
        if (controlsVisible) {
            drawPad(canvas, base);
        }
        for (Button b : buttons) {
            if (!controlsVisible && b.bit != MENU) continue;
            final boolean down = (held & b.bit) != 0;
            fill.setColor(Color.argb(down ? base * 3 / 4 : base / 3, 255, 255, 255));
            canvas.drawCircle(b.x, b.y, b.r, fill);
            stroke.setStrokeWidth(Math.max(2f, b.r * 0.06f));
            stroke.setColor(Color.argb(base, 255, 255, 255));
            canvas.drawCircle(b.x, b.y, b.r, stroke);
            text.setColor(Color.argb(Math.min(255, base + 60), down ? 30 : 255, down ? 30 : 255, down ? 30 : 255));
            text.setTextSize(b.label.length() > 2 ? b.r * 0.42f : b.r * 0.9f);
            canvas.drawText(b.label, b.x, b.y - (text.descent() + text.ascent()) / 2, text);
        }
    }

    private void drawPad(Canvas canvas, int base) {
        final float arm = padR * 0.36f;  // half the width of each bar
        fill.setColor(Color.argb(base / 3, 255, 255, 255));
        stroke.setStrokeWidth(Math.max(2f, padR * 0.025f));
        stroke.setColor(Color.argb(base, 255, 255, 255));
        rect.set(padX - padR, padY - arm, padX + padR, padY + arm);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, fill);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, stroke);
        rect.set(padX - arm, padY - padR, padX + arm, padY + padR);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, fill);
        canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, stroke);
        drawArrow(canvas, base, LEFT, -1, 0);
        drawArrow(canvas, base, RIGHT, 1, 0);
        drawArrow(canvas, base, UP, 0, -1);
        drawArrow(canvas, base, DOWN, 0, 1);
    }

    private void drawArrow(Canvas canvas, int base, int bit, int ux, int uy) {
        final boolean down = (held & bit) != 0;
        final float tipX = padX + ux * padR * 0.86f, tipY = padY + uy * padR * 0.86f;
        final float backX = padX + ux * padR * 0.56f, backY = padY + uy * padR * 0.56f;
        final float half = padR * 0.16f;
        arrow.reset();
        arrow.moveTo(tipX, tipY);
        arrow.lineTo(backX - uy * half, backY - ux * half);
        arrow.lineTo(backX + uy * half, backY + ux * half);
        arrow.close();
        fill.setColor(Color.argb(down ? Math.min(255, base + 80) : base, 255, 255, 255));
        canvas.drawPath(arrow, fill);
    }
}
