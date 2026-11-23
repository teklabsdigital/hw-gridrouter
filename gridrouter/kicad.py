"""Optional helper: write a Router's result into a pcbnew BOARD.

The pcbnew module is passed in by the caller; gridrouter itself never imports
it, so the router runs (and is tested) in any Python.
"""


def emit(board, router, netinfo, names, pcbnew, origin=(0.0, 0.0), via_dia=0.60,
         via_drill=0.30, layers=None):
    """Add router.routes() to board as PCB_TRACK / PCB_VIA objects.

    netinfo: {net name: NETINFO_ITEM}; names: {net number: net name};
    origin: board position of grid cell (0, 0) in mm (gen_pcb.py: (OX, OY));
    layers: (front, back) layer ids, default (pcbnew.F_Cu, pcbnew.B_Cu).
    Returns (number of segments, number of vias) like the reference emit().
    """
    MM = pcbnew.FromMM
    ox, oy = origin
    layer_id = layers or (pcbnew.F_Cu, pcbnew.B_Cu)

    def pt(x, y):
        return pcbnew.VECTOR2I(MM(ox + x), MM(oy + y))
    segs, vias = router.routes()
    for layer, x0, y0, x1, y1, w, net in segs:
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pt(x0, y0))
        t.SetEnd(pt(x1, y1))
        t.SetWidth(MM(w))
        t.SetLayer(layer_id[layer])
        t.SetNet(netinfo[names[net]])
        board.Add(t)
    for x, y, net in vias:
        v = pcbnew.PCB_VIA(board)
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetPosition(pt(x, y))
        v.SetLayerPair(layer_id[0], layer_id[1])
        v.SetDrill(MM(via_drill))
        v.SetWidth(layer_id[0], MM(via_dia))
        v.SetNet(netinfo[names[net]])
        board.Add(v)
    return len(segs), len(vias)
