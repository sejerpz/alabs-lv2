function (event, funcs) {
    /*
     * ExpSense modgui: a single status line that guides the calibration and then
     * shows its result, a tone indicator and a bar with the pedal value.
     * Calibrate is a trigger button: the calibration ends by itself and the
     * button stays lit while it runs. The result replaces the status for
     * RESULT_SHOW_MS, then the line goes back to the plugin status.
     */
    var RESULT_SHOW_MS = 10000;

    var STATUS = [
        "Uncalibrated",
        "Running",
        "Hold",
        "Cal: keep heel down…",
        "Cal: move to toe, hold…",
        "Cal: hold at toe…",
        "Cal: back to heel…"
    ];
    var ST_CAL_FIRST = 3, ST_CAL_LAST = 6;

    var RESULT = [
        "",
        "",
        "Cal: OK",
        "Cal: OK, In 2 level low",
        "Cal: swap Send/Return",
        "Cal: no signal",
        "Cal: In 2 clipping",
        "Cal: no latency, retry",
        "Cal: aborted",
        "Cal: toe not held, retry",
        "Cal: OK, check wiring"
    ];
    var RES_RUNNING = 1, RES_OK = 2;

    var d = event.data;
    if (d.status === undefined) {
        d.status = 0;
        d.result = 0;
        d.msg = "";
        d.msgGood = false;
        d.msgUntil = 0;
        d.timer = null;
    }

    function inCal(st) { return st >= ST_CAL_FIRST && st <= ST_CAL_LAST; }

    function render() {
        var icon = event.icon;
        var el = icon.find(".es-status");
        if (!inCal(d.status) && Date.now() < d.msgUntil) {
            el.text(d.msg);
            el.toggleClass("es-good", d.msgGood);
            el.toggleClass("es-bad", !d.msgGood);
        } else {
            el.text(STATUS[d.status] || String(d.status));
            el.removeClass("es-good es-bad");
        }
        icon.find('[mod-port-symbol="calibrate"]').toggleClass("es-busy", inCal(d.status));
    }

    function update(symbol, value, isStart) {
        var icon = event.icon;
        var prev, v;
        switch (symbol) {
        case "status":
            d.status = Math.round(value);
            if (inCal(d.status)) {
                d.msgUntil = 0; /* a new calibration takes over the line */
            }
            render();
            break;
        case "result":
            prev = d.result;
            d.result = Math.round(value);
            if (!isStart && prev === RES_RUNNING && d.result !== RES_RUNNING) {
                d.msg = RESULT[d.result] || String(d.result);
                d.msgGood = d.result === RES_OK;
                d.msgUntil = Date.now() + RESULT_SHOW_MS;
                if (d.timer) {
                    clearTimeout(d.timer);
                }
                d.timer = setTimeout(render, RESULT_SHOW_MS + 50);
            }
            render();
            break;
        case "value":
            v = Math.max(0, Math.min(1, value));
            icon.find(".es-bar-fill").css("width", (v * 100).toFixed(1) + "%");
            icon.find(".es-bar-text").text(v.toFixed(2));
            break;
        case "tone_active":
            v = Math.max(0, Math.min(1, value));
            icon.find(".es-tone").css("background", v > 0.05
                ? "rgba(255,177,61," + (0.35 + 0.65 * v).toFixed(2) + ")"
                : "#3a2508");
            break;
        }
    }

    if (event.type === "start") {
        for (var i = 0; i < event.ports.length; ++i) {
            update(event.ports[i].symbol, event.ports[i].value, true);
        }
    } else if (event.type === "change") {
        update(event.symbol, event.value, false);
    }
}
