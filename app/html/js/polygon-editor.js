'use strict';

const PolygonEditor = (function () {
    const DEFAULT_COLORS = ['#ef4444', '#10b981', '#f59e0b', '#3b82f6', '#8b5cf6'];
    const VERTEX_RADIUS = 9;
    const EDGE_HIT_RADIUS = 10;

    function clamp(value, min, max) {
        return Math.max(min, Math.min(max, value));
    }

    function canvasPoint(canvas, evt) {
        const rect = canvas.getBoundingClientRect();
        return {
            x: clamp(Math.round((evt.clientX - rect.left) * canvas.width / rect.width), 0, canvas.width),
            y: clamp(Math.round((evt.clientY - rect.top) * canvas.height / rect.height), 0, canvas.height)
        };
    }

    function segmentDistance(px, py, ax, ay, bx, by) {
        const dx = bx - ax;
        const dy = by - ay;
        const lenSq = dx * dx + dy * dy;
        if (!lenSq)
            return Math.hypot(px - ax, py - ay);
        const t = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / lenSq));
        const cx = ax + t * dx;
        const cy = ay + t * dy;
        return Math.hypot(px - cx, py - cy);
    }

    function pointInPolygon(poly, px, py) {
        let inside = false;
        for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
            const xi = poly[i].x;
            const yi = poly[i].y;
            const xj = poly[j].x;
            const yj = poly[j].y;
            if (((yi > py) !== (yj > py)) &&
                (px < (xj - xi) * (py - yi) / ((yj - yi) || 1e-9) + xi)) {
                inside = !inside;
            }
        }
        return inside;
    }

    function polygonCenter(poly) {
        let sx = 0;
        let sy = 0;
        poly.forEach(function (point) {
            sx += point.x;
            sy += point.y;
        });
        return {
            x: sx / poly.length,
            y: sy / poly.length
        };
    }

    function hitVertex(poly, px, py) {
        for (let i = 0; i < poly.length; i++) {
            if (Math.hypot(poly[i].x - px, poly[i].y - py) <= VERTEX_RADIUS + 3)
                return i;
        }
        return -1;
    }

    function hitEdge(poly, px, py) {
        for (let i = 0; i < poly.length; i++) {
            const next = (i + 1) % poly.length;
            if (segmentDistance(px, py, poly[i].x, poly[i].y, poly[next].x, poly[next].y) <= EDGE_HIT_RADIUS)
                return i;
        }
        return -1;
    }

    class Editor {
        constructor(canvas, options) {
            this.canvas = canvas;
            this.ctx = canvas.getContext('2d');
            this.colors = options.colors || DEFAULT_COLORS;
            this.hintEl = options.hintEl || null;
            this.onChanged = options.onChanged || null;
            this.onCommit = options.onCommit || null;
            this.polygons = [];
            this.selectedIndex = -1;
            this.drawMode = false;
            this.drawPoints = [];
            this.dragVertex = -1;
            this.dragPolygon = false;
            this.lastPoint = null;
            this.pointer = { x: 0, y: 0 };
            this.shiftPressed = false;
            this.extraDraw = null;
            this.invertFill = false;  // true = fill outside (AOI), false = fill inside (exclude)
            this.attachEvents();
        }

        setPolygons(polygons) {
            this.polygons = (polygons || []).map(function (item) {
                return {
                    id: item.id,
                    name: item.name,
                    active: item.active !== false,
                    polygon: (item.polygon || []).map(function (point) {
                        return { x: point.x, y: point.y };
                    })
                };
            });
            if (this.selectedIndex >= this.polygons.length)
                this.selectedIndex = -1;
            this.updateHint();
        }

        startDraw() {
            this.selectedIndex = -1;
            this.drawMode = true;
            this.drawPoints = [];
            this.canvas.classList.add('draw-mode');
            this.updateHint();
            this.redraw();
        }

        cancelDraw() {
            this.drawMode = false;
            this.drawPoints = [];
            this.dragVertex = -1;
            this.dragPolygon = false;
            this.canvas.classList.remove('draw-mode');
            this.updateHint();
            this.redraw();
        }

        redraw(extraDraw) {
            if (extraDraw)
                this.extraDraw = extraDraw;

            const ctx = this.ctx;
            ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);

            if (this.extraDraw)
                this.extraDraw(ctx);

            this.polygons.forEach((item, index) => {
                if (!item.polygon || item.polygon.length < 3)
                    return;
                const color = this.colors[index % this.colors.length];
                const selected = index === this.selectedIndex;

                ctx.beginPath();
                ctx.moveTo(item.polygon[0].x, item.polygon[0].y);
                for (let i = 1; i < item.polygon.length; i++) {
                    ctx.lineTo(item.polygon[i].x, item.polygon[i].y);
                }
                ctx.closePath();

                if (this.invertFill) {
                    // Fill the area OUTSIDE the polygon (dimmed overlay with hole)
                    if (item.active) {
                        ctx.save();
                        ctx.beginPath();
                        ctx.rect(0, 0, this.canvas.width, this.canvas.height);
                        ctx.moveTo(item.polygon[0].x, item.polygon[0].y);
                        for (let i = 1; i < item.polygon.length; i++) {
                            ctx.lineTo(item.polygon[i].x, item.polygon[i].y);
                        }
                        ctx.closePath();
                        ctx.fillStyle = 'rgba(0,0,0,0.45)';
                        ctx.fill('evenodd');
                        ctx.restore();
                    }
                } else {
                    ctx.fillStyle = item.active ? color + '55' : color + '22';
                    ctx.fill();
                }

                ctx.beginPath();
                ctx.moveTo(item.polygon[0].x, item.polygon[0].y);
                for (let i = 1; i < item.polygon.length; i++) {
                    ctx.lineTo(item.polygon[i].x, item.polygon[i].y);
                }
                ctx.closePath();
                ctx.strokeStyle = selected ? '#ffffff' : color;
                ctx.lineWidth = selected ? 4 : 2.5;
                ctx.stroke();

                if (selected) {
                    item.polygon.forEach((point, vertexIndex) => {
                        const hovered = hitVertex(item.polygon, this.pointer.x, this.pointer.y) === vertexIndex;
                        ctx.beginPath();
                        ctx.arc(point.x, point.y, hovered ? VERTEX_RADIUS + 2 : VERTEX_RADIUS, 0, Math.PI * 2);
                        ctx.fillStyle = hovered ? '#ffffff' : color;
                        ctx.strokeStyle = hovered ? color : '#ffffff';
                        ctx.lineWidth = 2;
                        ctx.fill();
                        ctx.stroke();
                    });
                }
            });

            if (this.drawMode && this.drawPoints.length) {
                ctx.setLineDash([8, 5]);
                ctx.strokeStyle = '#ffffff';
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.moveTo(this.drawPoints[0].x, this.drawPoints[0].y);
                for (let i = 1; i < this.drawPoints.length; i++) {
                    ctx.lineTo(this.drawPoints[i].x, this.drawPoints[i].y);
                }
                ctx.lineTo(this.pointer.x, this.pointer.y);
                ctx.stroke();
                ctx.setLineDash([]);

                this.drawPoints.forEach((point, index) => {
                    ctx.beginPath();
                    ctx.arc(point.x, point.y, index === 0 ? 8 : 6, 0, Math.PI * 2);
                    ctx.fillStyle = index === 0 ? '#22c55e' : '#ffffff';
                    ctx.strokeStyle = '#1f2937';
                    ctx.lineWidth = 2;
                    ctx.fill();
                    ctx.stroke();
                });
            }
        }

        updateHint() {
            if (!this.hintEl)
                return;

            if (this.drawMode) {
                this.hintEl.textContent = 'Click to place vertices. Click the first point or double-click to close the polygon. Esc cancels.';
                return;
            }
            if (this.selectedIndex >= 0) {
                this.hintEl.textContent = 'Drag vertices to edit. Shift+click an edge to insert a point. Right-click a vertex to remove it.';
                return;
            }
            this.hintEl.textContent = 'Select AOI or Exclusion, then draw polygons on top of the live video.';
        }

        notifyChanged() {
            if (this.onChanged)
                this.onChanged(this.polygons);
        }

        commitPolygon() {
            if (this.drawPoints.length < 3) {
                alert('A polygon needs at least 3 points.');
                return;
            }
            const polygon = this.drawPoints.map(function (point) {
                return { x: point.x, y: point.y };
            });
            this.drawMode = false;
            this.drawPoints = [];
            this.canvas.classList.remove('draw-mode');
            this.updateHint();
            if (this.onCommit)
                this.onCommit(polygon);
        }

        attachEvents() {
            this.canvas.addEventListener('contextmenu', (evt) => {
                evt.preventDefault();
                if (this.drawMode || this.selectedIndex < 0)
                    return;

                const point = canvasPoint(this.canvas, evt);
                const item = this.polygons[this.selectedIndex];
                if (!item || !item.polygon)
                    return;

                const vertexIndex = hitVertex(item.polygon, point.x, point.y);
                if (vertexIndex < 0 || item.polygon.length <= 3)
                    return;

                item.polygon.splice(vertexIndex, 1);
                this.redraw();
                this.notifyChanged();
            });

            this.canvas.addEventListener('mousemove', (evt) => {
                const point = canvasPoint(this.canvas, evt);
                this.pointer = point;
                this.shiftPressed = evt.shiftKey;

                if (this.dragVertex >= 0 && this.selectedIndex >= 0) {
                    const item = this.polygons[this.selectedIndex];
                    item.polygon[this.dragVertex] = point;
                    this.redraw();
                    return;
                }

                if (this.dragPolygon && this.selectedIndex >= 0 && this.lastPoint) {
                    let dx = point.x - this.lastPoint.x;
                    let dy = point.y - this.lastPoint.y;
                    const item = this.polygons[this.selectedIndex];
                    // Clamp the delta so no vertex leaves the canvas
                    item.polygon.forEach((vertex) => {
                        if (vertex.x + dx < 0) dx = -vertex.x;
                        if (vertex.x + dx > this.canvas.width) dx = this.canvas.width - vertex.x;
                        if (vertex.y + dy < 0) dy = -vertex.y;
                        if (vertex.y + dy > this.canvas.height) dy = this.canvas.height - vertex.y;
                    });
                    item.polygon.forEach(function (vertex) {
                        vertex.x += dx;
                        vertex.y += dy;
                    });
                    this.lastPoint = point;
                    this.redraw();
                    return;
                }

                this.redraw();
            });

            this.canvas.addEventListener('mousedown', (evt) => {
                if (evt.button !== 0 || this.drawMode)
                    return;

                const point = canvasPoint(this.canvas, evt);
                if (this.selectedIndex >= 0) {
                    const item = this.polygons[this.selectedIndex];
                    const vertexIndex = hitVertex(item.polygon, point.x, point.y);
                    if (vertexIndex >= 0) {
                        this.dragVertex = vertexIndex;
                        return;
                    }

                    if (evt.shiftKey) {
                        const edgeIndex = hitEdge(item.polygon, point.x, point.y);
                        if (edgeIndex >= 0) {
                            item.polygon.splice(edgeIndex + 1, 0, point);
                            this.dragVertex = edgeIndex + 1;
                            this.redraw();
                            this.notifyChanged();
                            return;
                        }
                    }

                    if (pointInPolygon(item.polygon, point.x, point.y)) {
                        this.dragPolygon = true;
                        this.lastPoint = point;
                        return;
                    }
                }

                this.selectedIndex = -1;
                for (let i = 0; i < this.polygons.length; i++) {
                    if (this.polygons[i].polygon && pointInPolygon(this.polygons[i].polygon, point.x, point.y)) {
                        this.selectedIndex = i;
                        break;
                    }
                }
                this.updateHint();
                this.redraw();
            });

            // Use document-level mouseup so releasing outside the canvas stops dragging
            document.addEventListener('mouseup', () => {
                const changed = this.dragVertex >= 0 || this.dragPolygon;
                this.dragVertex = -1;
                this.dragPolygon = false;
                this.lastPoint = null;
                if (changed)
                    this.notifyChanged();
            });

            this.canvas.addEventListener('click', (evt) => {
                if (!this.drawMode)
                    return;

                const point = canvasPoint(this.canvas, evt);
                if (this.drawPoints.length >= 3) {
                    const first = this.drawPoints[0];
                    if (Math.hypot(first.x - point.x, first.y - point.y) <= 12) {
                        this.commitPolygon();
                        return;
                    }
                }
                this.drawPoints.push(point);
                this.redraw();
            });

            this.canvas.addEventListener('dblclick', () => {
                if (!this.drawMode)
                    return;
                if (this.drawPoints.length >= 3)
                    this.commitPolygon();
            });

            document.addEventListener('keydown', (evt) => {
                if (evt.key === 'Escape') {
                    if (this.drawMode) {
                        this.cancelDraw();
                    } else if (this.selectedIndex >= 0) {
                        this.selectedIndex = -1;
                        this.updateHint();
                        this.redraw();
                    }
                }
            });
        }
    }

    return Editor;
})();