package org.sims2.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.StateListDrawable;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Comparator;
import java.util.function.BiConsumer;
import java.util.function.BooleanSupplier;

/** The choice sits beside its targets and leaves the scene visible. */
final class WorldTargetMenu extends FrameLayout {
    private final float density;
    private final BooleanSupplier current;
    private final ScrollView column;
    private final Button[] rows;
    private final float[] targetX, targetY;
    private final boolean onRight;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path lead = new Path();
    private boolean confirmPressed;

    WorldTargetMenu(Context context, int width, int height, Rect safeArea, long gameSize,
            GameActivity.WorldTarget[] targets, BooleanSupplier current,
            BiConsumer<WorldTargetMenu, GameActivity.WorldTarget> select) {
        super(context);
        this.current = current;
        density = getResources().getDisplayMetrics().density;
        setFocusable(false);
        setWillNotDraw(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        setAccessibilityPaneTitle(GameActivity.text(R.string.choose_target));
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);

        Arrays.sort(targets, Comparator.comparingInt((GameActivity.WorldTarget target) -> target.key == -1 ? 1 : 0)
                .thenComparingInt(target -> target.anchorY).thenComparingInt(target -> target.anchorX));
        rows = new Button[targets.length];
        targetX = new float[targets.length];
        targetY = new float[targets.length];
        int gameWidth = (int)gameSize, gameHeight = (int)(gameSize >>> 32);
        float scale = Math.min(width / (float)gameWidth, height / (float)gameHeight);
        float offsetX = (width - gameWidth * scale) * .5f, offsetY = (height - gameHeight * scale) * .5f;
        float leftmost = width, rightmost = 0, topmost = height, bottommost = 0;
        int preferredWidth = dp(134), rowHeight = dp(48);
        LinearLayout items = new LinearLayout(context);
        items.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < targets.length; i++) {
            GameActivity.WorldTarget target = targets[i];
            targetX[i] = offsetX + target.anchorX * scale;
            targetY[i] = offsetY + target.anchorY * scale;
            leftmost = Math.min(leftmost, targetX[i]);
            rightmost = Math.max(rightmost, targetX[i]);
            topmost = Math.min(topmost, targetY[i]);
            bottommost = Math.max(bottommost, targetY[i]);
            String label = target.key == -1 ? GameActivity.text(R.string.target_ground)
                    : new String(target.name, StandardCharsets.UTF_8);
            if (label.trim().isEmpty()) label = GameActivity.text(R.string.target_object, i + 1);
            Button row = new Button(context);
            row.setText(label);
            row.setAllCaps(false);
            row.setTextSize(14);
            row.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            row.setTextColor(0xfff2f2ff);
            row.setShadowLayer(dp(1), 0, dp(1), 0xff252052);
            row.setGravity(Gravity.CENTER);
            row.setMaxLines(2);
            row.setEllipsize(TextUtils.TruncateAt.END);
            row.setFocusable(true);
            row.setDefaultFocusHighlightEnabled(false);
            row.setStateListAnimator(null);
            row.setBackgroundTintList(null);
            StateListDrawable background = new StateListDrawable();
            background.addState(new int[]{android.R.attr.state_pressed}, rowBackground(true));
            background.addState(new int[]{android.R.attr.state_focused}, rowBackground(true));
            background.addState(new int[0], rowBackground(false));
            row.setBackground(background);
            row.setPadding(dp(13), dp(4), dp(13), dp(4));
            row.setOnClickListener(v -> select.accept(this, target));
            row.setOnTouchListener((v, event) -> { invalidate(); return false; });
            row.setOnFocusChangeListener((v, focused) -> invalidate());
            rows[i] = row;
            preferredWidth = Math.max(preferredWidth, (int)Math.ceil(row.getPaint().measureText(label)) + dp(30));
            rowHeight = Math.max(rowHeight, row.getLineHeight() * 2 + dp(10));
        }
        for (Button row : rows) items.addView(row, new LinearLayout.LayoutParams(-1, rowHeight));

        int left = safeArea.left + dp(12), right = safeArea.right - dp(12);
        int top = safeArea.top + dp(8), bottom = safeArea.bottom - dp(8);
        int leftSpace = (int)leftmost - left, rightSpace = right - (int)Math.ceil(rightmost);
        onRight = rightSpace >= leftSpace;
        int space = onRight ? rightSpace : leftSpace;
        int listWidth = Math.max(1, Math.min(Math.min(dp(270), preferredWidth),
                Math.min(right - left, Math.max(dp(134), space - dp(52)))));
        int gap = Math.min(dp(52), Math.max(0, space - listWidth));
        int contentHeight = rowHeight * rows.length;
        int listHeight = Math.max(1, Math.min(bottom - top, contentHeight));
        float centerY = (topmost + bottommost) * .5f;
        int x = onRight ? (int)Math.ceil(rightmost) + gap : (int)leftmost - gap - listWidth;
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(listWidth, listHeight, Gravity.TOP | Gravity.LEFT);
        layout.leftMargin = Math.max(left, Math.min(right - listWidth, x));
        layout.topMargin = Math.max(top, Math.min(bottom - listHeight, Math.round(centerY - listHeight * .5f)));
        column = new ScrollView(context);
        column.setFocusable(false);
        column.setOverScrollMode(OVER_SCROLL_NEVER);
        column.setVerticalScrollBarEnabled(contentHeight > listHeight);
        if (contentHeight > listHeight) {
            GradientDrawable thumb = new GradientDrawable();
            thumb.setColor(0xffa6aaf2);
            thumb.setCornerRadius(dp(2));
            column.setVerticalScrollbarThumbDrawable(thumb);
            column.setScrollBarSize(dp(3));
            column.setScrollbarFadingEnabled(false);
        }
        column.setOnScrollChangeListener((v, sx, sy, oldX, oldY) -> invalidate());
        column.addView(items);
        addView(column, layout);
    }

    boolean isCurrent() { return current.getAsBoolean(); }

    boolean handlePadKey(KeyEvent event) {
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_ESCAPE) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) performClick();
            return true;
        }
        if (code != KeyEvent.KEYCODE_BUTTON_A) return false;
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) confirmPressed = true;
        if (event.getAction() == KeyEvent.ACTION_UP && confirmPressed) {
            confirmPressed = false;
            if (!event.isCanceled()) for (Button row : rows) if (row.isFocused()) { row.performClick(); break; }
        }
        return true;
    }

    private int dp(float value) { return Math.round(value * density); }

    private InsetDrawable rowBackground(boolean highlighted) {
        int[] colors = highlighted ? new int[]{0xffbadfba, 0xff526f6b, 0xff304d56, 0xff64898b}
                : new int[]{0xffa6aaf2, 0xff5559a8, 0xff363678, 0xff6a6fc2};
        GradientDrawable fill = new GradientDrawable();
        fill.setColors(colors, new float[]{0f, .19f, .72f, 1f});
        fill.setOrientation(GradientDrawable.Orientation.TOP_BOTTOM);
        fill.setCornerRadius(dp(16));
        fill.setStroke(dp(1), highlighted ? 0xffc5ebb1 : 0xff292756);
        return new InsetDrawable(fill, 0, dp(6), 0, dp(6));
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int highlighted = -1;
        for (int i = 0; i < rows.length; i++) {
            if (rows[i].isPressed() || rows[i].isFocused()) highlighted = i;
            else drawLead(canvas, i, false);
        }
        if (highlighted != -1) drawLead(canvas, highlighted, true);
    }

    private void drawLead(Canvas canvas, int index, boolean highlighted) {
        float rowY = column.getTop() + rows[index].getTop() + rows[index].getHeight() * .5f - column.getScrollY();
        if (rowY <= column.getTop() || rowY >= column.getBottom()) return;
        float edge = onRight ? column.getLeft() : column.getRight();
        float x = targetX[index], y = targetY[index];
        lead.reset();
        lead.moveTo(edge, rowY);
        lead.lineTo(edge + (x - edge) * .25f, rowY);
        lead.lineTo(x, y);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth((highlighted ? 3.4f : 2.4f) * density);
        paint.setColor(0xd41d244d);
        canvas.drawPath(lead, paint);
        int accent = highlighted ? 0xffd1f3b8 : 0xffaeb5ef;
        paint.setColor(accent);
        paint.setStrokeWidth((highlighted ? 1.7f : 1.05f) * density);
        canvas.drawPath(lead, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xff252f5e);
        canvas.drawCircle(x, y, (highlighted ? 3.2f : 1.9f) * density, paint);
        paint.setColor(accent);
        canvas.drawCircle(x, y, (highlighted ? 2.1f : 1.1f) * density, paint);
    }
}
