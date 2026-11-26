package org.battlerealms.nativeport;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** Small, movable in-game controls. Only the buttons consume touches; the game surface stays touch-through. */
final class GameTouchHud {
    private static final String PREFS = "battle_realms_touch_hud";
    private static final String POSITIONS = "positions";
    private static final String HIDDEN = "hidden";
    private static final String OPACITY = "opacity";
    private static final String SCALE = "scale";

    private final GameActivity activity;
    private final FrameLayout overlay;
    private final android.content.SharedPreferences prefs;
    private final List<Control> controls = new ArrayList<>();
    private boolean editing;
    private boolean hidden;
    private boolean rightTapMode;

    GameTouchHud(GameActivity activity, FrameLayout root) {
        this.activity = activity;
        this.prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        this.hidden = prefs.getBoolean(HIDDEN, false);
        this.rightTapMode = prefs.getBoolean("right_tap_mode", false);
        this.overlay = new FrameLayout(activity);
        overlay.setClipChildren(false);
        overlay.setClipToPadding(false);
        overlay.setFocusable(false);
        root.addView(overlay, new FrameLayout.LayoutParams(-1, -1));

        add("RMB", 1, "rmb", .94f, .68f);
        add("ESC", 2, "esc", .94f, .82f);
        add("−", 4, "zoom_out", .86f, .82f);
        add("+", 3, "zoom_in", .86f, .68f);
        add(rightTapMode ? "TAP:R" : "TAP:L", 5, "tap_mode", .78f, .82f);
        add("EDIT", -1, "edit", .08f, .82f);
        add(hidden ? "SHOW" : "HUD", -2, "hide", .08f, .68f);
        overlay.post(this::layoutControls);
        LaunchDiagnostics.event("TOUCH_HUD_ATTACHED", "controls=%d overlay=activity_content", controls.size());
    }

    void detach() {
        if (overlay.getParent() instanceof FrameLayout) ((FrameLayout) overlay.getParent()).removeView(overlay);
        controls.clear();
    }

    private int dp(float value) { return Math.round(value * activity.getResources().getDisplayMetrics().density); }

    private void add(String text, int action, String id, float defaultX, float defaultY) {
        Control control = new Control(id, action, defaultX, defaultY);
        TextView button = new TextView(activity);
        control.view = button;
        button.setText(text);
        button.setTextColor(Color.WHITE);
        button.setTextSize(12f);
        button.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(6), dp(3), dp(6), dp(3));
        button.setMinWidth(dp(58));
        button.setMinHeight(dp(42));
        button.setContentDescription(description(id));
        button.setBackground(background(id.equals("tap_mode") && rightTapMode));
        button.setAlpha(prefs.getInt(OPACITY, 82) / 100f);
        button.setElevation(dp(4));
        button.setClickable(true);
        button.setOnClickListener(v -> activate(control));
        button.setOnTouchListener((v, event) -> editTouch(control, event));
        controls.add(control);
        overlay.addView(button);
    }

    private String description(String id) {
        switch (id) {
            case "rmb": return "Right click or issue the current context order";
            case "esc": return "Cancel or close the current game screen";
            case "zoom_in": return "Send mouse wheel up at the last game cursor position";
            case "zoom_out": return "Send mouse wheel down at the last game cursor position";
            case "tap_mode": return "Toggle whether a single tap sends left or right click";
            case "edit": return "Move floating game controls";
            default: return hidden ? "Show floating game controls" : "Hide floating game controls";
        }
    }

    private GradientDrawable background(boolean selected) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.RECTANGLE);
        shape.setCornerRadius(dp(10));
        shape.setColor(selected ? Color.argb(185, 38, 112, 166) : Color.argb(168, 25, 29, 36));
        shape.setStroke(dp(1), Color.argb(180, 228, 229, 228));
        return shape;
    }

    private void activate(Control c) {
        if (editing) return;
        if (c.action > 0) {
            if (c.id.equals("tap_mode")) {
                rightTapMode = !rightTapMode;
                prefs.edit().putBoolean("right_tap_mode", rightTapMode).apply();
                c.view.setText(rightTapMode ? "TAP:R" : "TAP:L");
                c.view.setBackground(background(rightTapMode));
            }
            GameActivity.nativeGameTouchAction(c.action);
        } else if (c.id.equals("edit")) {
            editing = true;
            c.view.setText("DONE");
            c.view.setBackground(background(true));
        } else {
            hidden = !hidden;
            prefs.edit().putBoolean(HIDDEN, hidden).apply();
            for (Control item : controls) {
                if (item.id.equals("hide")) item.view.setText(hidden ? "SHOW" : "HUD");
                else if (!item.id.equals("edit")) item.view.setVisibility(hidden ? View.GONE : View.VISIBLE);
            }
        }
    }

    private boolean editTouch(Control c, MotionEvent event) {
        if (!editing) return false;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                c.downRawX = event.getRawX(); c.downRawY = event.getRawY();
                c.startX = c.x; c.startY = c.y; c.dragged = false;
                return true;
            case MotionEvent.ACTION_MOVE:
                float dx = event.getRawX() - c.downRawX, dy = event.getRawY() - c.downRawY;
                int slop = ViewConfiguration.get(activity).getScaledTouchSlop();
                if (!c.dragged && (Math.abs(dx) > slop || Math.abs(dy) > slop)) c.dragged = true;
                if (c.dragged) {
                    float width = Math.max(1, overlay.getWidth() - c.view.getWidth());
                    float height = Math.max(1, overlay.getHeight() - c.view.getHeight());
                    c.x = clamp(c.startX + dx / width); c.y = clamp(c.startY + dy / height);
                    layout(c);
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (c.dragged) savePositions();
                else if (c.id.equals("edit")) {
                    editing = false;
                    c.view.setText("EDIT");
                    c.view.setBackground(background(false));
                } else activate(c);
                return true;
            case MotionEvent.ACTION_CANCEL:
                c.x = c.startX; c.y = c.startY; layout(c); return true;
            default: return true;
        }
    }

    private void layoutControls() {
        if (overlay.getWidth() == 0 || overlay.getHeight() == 0) return;
        int scale = Math.max(70, Math.min(150, prefs.getInt(SCALE, 100)));
        for (Control c : controls) {
            c.view.setTextSize(12f * scale / 100f);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(66 * scale / 100f), dp(46 * scale / 100f));
            lp.gravity = Gravity.TOP | Gravity.LEFT;
            c.view.setLayoutParams(lp);
            if (!prefs.contains("pos_" + c.id + "_x")) { c.x = c.defaultX; c.y = c.defaultY; }
            else {
                c.x = prefs.getFloat("pos_" + c.id + "_x", c.defaultX);
                c.y = prefs.getFloat("pos_" + c.id + "_y", c.defaultY);
            }
            layout(c);
            if (hidden && !c.id.equals("hide") && !c.id.equals("edit")) c.view.setVisibility(View.GONE);
        }
    }

    private void layout(Control c) {
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) c.view.getLayoutParams();
        lp.leftMargin = Math.round(c.x * Math.max(0, overlay.getWidth() - c.view.getWidth()));
        lp.topMargin = Math.round(c.y * Math.max(0, overlay.getHeight() - c.view.getHeight()));
        c.view.setLayoutParams(lp);
    }

    private void savePositions() {
        android.content.SharedPreferences.Editor editor = prefs.edit();
        for (Control c : controls) {
            editor.putFloat("pos_" + c.id + "_x", clamp(c.x));
            editor.putFloat("pos_" + c.id + "_y", clamp(c.y));
        }
        editor.apply();
    }

    private static float clamp(float v) { return Math.max(0f, Math.min(1f, v)); }

    private static final class Control {
        final String id; final int action; final float defaultX, defaultY; TextView view;
        float x, y, startX, startY, downRawX, downRawY; boolean dragged;
        Control(String id, int action, float x, float y) { this.id = id; this.action = action; this.defaultX = x; this.defaultY = y; this.x = x; this.y = y; }
    }
}
