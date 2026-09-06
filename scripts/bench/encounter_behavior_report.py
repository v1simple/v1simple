"""Write the ordinary visual-behavior report from measured event evidence.

The report never reads pixels, changes a judgment or manufactures a response
deadline. It keeps expected input and independently observed content separate.
"""
from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path


_WITNESS_KEYS = ("frame_id", "frame_index", "video_frame_index", "source_frame_seq",
                 "capture_ns", "image", "observed", "comparison_status")


def _witness(value):
    if not isinstance(value, dict):
        return value
    return {key: deepcopy(value[key]) for key in _WITNESS_KEYS if key in value}


def _interval(value):
    return {"first": _witness(value.get("first")), "last": _witness(value.get("last")),
            "frame_count": value.get("frame_count", 0)}


def _observation(value):
    """Omit duplicate lists and pixel profiles, retaining each uncertainty span."""
    result = {key: deepcopy(value[key]) for key in (
        "input_anchor_ns", "target_observed", "target_already_correct_in_preceding_observation", "coverage",
        "timing", "first_target_ms")
              if key in value}
    result["first_target_observation"] = _witness(value.get("first_target_observation"))
    result["fields"] = {}
    for name, field in value.get("fields", {}).items():
        item = {"target_observed": field.get("target_observed"),
                "first_target_observation": _witness(field.get("first_target_observation")),
                "latest_contrary_observation": _witness(field.get("latest_contrary_observation")),
                "counts": deepcopy(field.get("counts", {})),
                "unresolved_intervals": [_interval(interval) for interval in field.get("unresolved_intervals", [])],
                "difference_intervals": [_interval(interval) for interval in field.get("difference_intervals", [])],
                "post_target_different_frames": sum(interval.get("frame_count", 0)
                    for interval in field.get("post_target_departures", []))}
        bracket = field.get("first_target_capture_bracket")
        item["first_target_capture_bracket"] = ({
            "preceding_observation": _witness(bracket.get("preceding_observation")),
            "last_definite_difference": _witness(bracket.get("last_definite_difference")),
            "first_target": _witness(bracket.get("first_target")),
            "marker_separation_ms": bracket.get("marker_separation_ms"),
            "intervening_unresolved_frames": bracket.get("intervening_unresolved_frames", 0),
            "recorded_frame_gaps": deepcopy(bracket.get("recorded_frame_gaps", [])),
        } if bracket else None)
        end = field.get("end_state", {})
        item["end_state"] = {"last_observation": _witness(end.get("last_observation")),
                             "last_definite_observation": _witness(end.get("last_definite_observation")),
                             "last_definite_matches_target": end.get("last_definite_matches_target"),
                             "unresolved_suffix": [_interval(interval)
                                 for interval in end.get("unresolved_suffix", [])],
                             "last_capture_to_event_end_ms": end.get("last_capture_to_event_end_ms")}
        result["fields"][name] = item
    return result


def _payload(result):
    payload = {key: deepcopy(result[key]) for key in (
        "schema_version", "kind", "evidence", "reader_method", "behavior_contract", "summary",
        "comparison", "coverage", "scope", "errors", "status", "result", "persistence") if key in result}
    payload["events"] = []
    for event in result.get("events", []):
        item = {key: deepcopy(event[key]) for key in (
            "event_id", "input_key", "input", "wire_rows", "target", "findings", "coverage",
            "start_ns", "end_ns", "outcome", "status", "comparison", "phase_observation") if key in event}
        item["observation"] = _observation(event.get("observation", {}))
        item["observation_spans"] = [{"first": _witness(span.get("first")),
            "last": _witness(span.get("last")), "frame_count": span.get("frame_count"),
            "judgment": deepcopy(span.get("judgment", {})),
            "observed": {name: {key: deepcopy(reading[key]) for key in ("state", "value", "reason")
                                if key in reading}
                         for name, reading in span.get("observed", {}).items()}}
            for span in event.get("observation_spans", [])]
        payload["events"].append(item)
    payload["samples_index"] = [{key: deepcopy(sample[key]) for key in (
        "frame_index", "video_frame_index", "capture_ns", "image", "event_id", "video_seconds") if key in sample}
        for sample in result.get("samples_index", [])]
    return payload


def write_behavior_report(out: Path, result: dict) -> Path:
    """Write one self-contained report and return its path.

    Original images remain separate unchanged files referenced by the supplied
    sample index. ``out`` can be an output directory or an explicit HTML path.
    """
    out = Path(out)
    destination = out if out.suffix.lower() == ".html" else out / "report.html"
    destination.parent.mkdir(parents=True, exist_ok=True)
    data = json.dumps(_payload(result), ensure_ascii=True, separators=(",", ":"))
    # Report content includes arbitrary observed text. A literal closing script
    # or HTML tag must never turn that data into executable markup.
    data = data.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    destination.write_text(_HTML.replace("__RESULT_JSON__", data), encoding="utf-8")
    return destination


_HTML = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware visual behavior</title><style>
:root{color-scheme:light;--ink:#18292d;--muted:#596b70;--line:#d6e0df;--paper:#fff;--wash:#eef3f2;--accent:#006860;--warn:#945b00;--bad:#a63223}
*{box-sizing:border-box}body{margin:0;background:var(--wash);color:var(--ink);font:15px/1.48 system-ui,-apple-system,sans-serif}header{padding:28px 30px 18px;background:var(--paper);border-bottom:1px solid var(--line)}h1{font-size:28px;margin:0 0 5px;letter-spacing:-.6px}h2{font-size:21px;margin:0 0 12px}h3{font-size:16px;margin:18px 0 8px}p{margin:8px 0}a{color:var(--accent);text-underline-offset:3px}button,input,select{font:inherit}button{cursor:pointer;background:#fff;border:1px solid #b9c9c7;border-radius:6px;padding:5px 9px;color:var(--accent)}button:hover{background:#e6f3ef}button:disabled{opacity:.4;cursor:default}button:focus-visible,a:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #d29522;outline-offset:2px}.muted{color:var(--muted)}.small{font-size:13px}.identity{display:flex;gap:20px;flex-wrap:wrap;margin:14px 0 12px}.identity strong{font-variant-numeric:tabular-nums}.stats{display:grid;grid-template-columns:repeat(4,minmax(140px,1fr));gap:10px;margin-top:16px}.stat{padding:12px 15px;background:var(--wash);border:1px solid var(--line);border-radius:8px}.stat b{font-size:26px;display:block;line-height:1.3}.stat span{font-size:13px}.layout{display:grid;grid-template-columns:280px minmax(0,1fr);max-width:1700px;margin:0 auto;align-items:start}.sidebar{position:sticky;top:0;max-height:100vh;display:flex;flex-direction:column;padding:18px 12px;border-right:1px solid var(--line)}.filters{display:grid;gap:8px;padding:0 4px 10px}.filters input,.filters select{width:100%;padding:8px;border:1px solid #b9c9c7;border-radius:6px;background:#fff}.event-list{overflow-y:auto;padding:0 4px}.event-button{display:block;text-align:left;width:100%;margin:0 0 6px;padding:10px;color:var(--ink)}.event-button.active{border-color:var(--accent);box-shadow:inset 3px 0 var(--accent);background:#e4f1ec}.event-button b{font-size:13px}.event-button span{display:block;font-size:12px;margin-top:3px;overflow-wrap:anywhere}.tag{display:inline-block!important;border-radius:10px;padding:1px 7px;background:#e5eeec;color:var(--accent);font-size:11px!important;width:max-content}.tag.bad{background:#fae6e1;color:var(--bad)}.tag.warn{background:#fff0cc;color:var(--warn)}main{padding:22px;min-width:0}.panel{background:var(--paper);border:1px solid var(--line);border-radius:9px;padding:20px;margin-bottom:16px}.event-heading{display:flex;align-items:start;justify-content:space-between;gap:15px}.facts{display:grid;grid-template-columns:1fr 1fr;gap:16px}.facts>div{min-width:0}.literal{white-space:pre-wrap;overflow-wrap:anywhere;font:13px/1.5 ui-monospace,SFMono-Regular,monospace;margin:4px 0}.reading{padding:10px;background:var(--wash);border-radius:6px}.finding{border-left:3px solid var(--bad);padding:10px 14px;background:#fff8f5;margin:12px 0}.finding .facts{margin:10px 0}.finding h3{margin:0 0 6px}.finding.neutral{border-color:var(--warn);background:#fffbf2}.witness{white-space:nowrap;font-size:12px;padding:3px 6px;margin:2px}.table-wrap{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:13px}th,td{text-align:left;vertical-align:top;padding:10px 8px;border-bottom:1px solid var(--line)}th{font-weight:600;color:var(--muted)}td:first-child{font-weight:600}details{margin:10px 0}summary{cursor:pointer;color:var(--accent);padding:3px 0}pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.5 ui-monospace,monospace;background:var(--wash);padding:12px;border-radius:6px}.viewer-top{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}.viewer-image{width:100%;max-height:590px;object-fit:contain;background:#0b1112;display:block;margin:12px 0;border-radius:6px}.viewer-controls{display:flex;align-items:center;gap:10px}.viewer-controls input{flex:1;min-width:50px}.viewer-label{font-variant-numeric:tabular-nums}.notice{padding:10px 12px;border-left:3px solid var(--warn);background:#fff5dc}.rule{padding:10px 0;border-top:1px solid var(--line)}.source-links a{display:inline-block;font-size:12px;margin-right:10px}.empty{padding:30px 10px;color:var(--muted)}footer{padding:12px 30px 25px;font-size:12px;color:var(--muted)}
@media(max-width:900px){.layout{grid-template-columns:220px minmax(0,1fr)}.stats{grid-template-columns:repeat(2,1fr)}main{padding:14px}.panel{padding:14px}.facts{grid-template-columns:1fr}}
@media(max-width:650px){header{padding:20px 16px}.layout{display:block}.sidebar{position:relative;max-height:300px;border-right:0;border-bottom:1px solid var(--line)}.event-list{display:flex;gap:8px;overflow:auto}.event-button{min-width:170px}.filters{grid-template-columns:1fr 1fr}.filters .small{grid-column:1/-1}.stats{gap:6px}.stat{padding:9px}.stat b{font-size:23px}.viewer-controls{gap:5px}}
</style></head><body>
<header><h1>Firmware visual behavior</h1><p class="muted">Known replay inputs, independently read physical display, and evidence for each finding.</p><p id="overall-result"></p>
<div class="identity" id="identity"></div><div class="stats" id="stats"></div><p class="small muted" id="coverage-summary"></p>
<details><summary>Tested scope, settings and method</summary><div id="method"></div></details><div id="global-errors"></div>
</header><div class="layout"><aside class="sidebar" aria-label="Event index"><div class="filters">
<input id="search" type="search" aria-label="Search events" placeholder="Search input, field or event…">
<select id="filter" aria-label="Filter events"><option value="all">All events</option><option value="findings">With findings</option><option value="unknown">With unreadable observations</option><option value="missing">Complete target not seen</option><option value="observed">Complete target seen</option></select>
<div class="small muted" id="list-count"></div></div><div class="event-list" id="event-list"></div></aside>
<main><div id="comparison"></div><section class="panel" id="event-content"></section>
<section class="panel" id="viewer"><div class="viewer-top"><h2>Original camera evidence</h2><a id="original-link" target="_blank" rel="noopener">Open unchanged original image ↗</a></div>
<p class="small muted" id="viewer-context"></p><img id="frame-image" class="viewer-image" alt="Select an original frame">
<div class="viewer-controls"><button id="prev-frame" aria-label="Previous retained witness">←</button><input id="frame-slider" type="range" min="0" max="0" value="0" aria-label="Retained original witnesses"><button id="next-frame" aria-label="Next retained witness">→</button></div>
<p class="small viewer-label" id="frame-label"></p><p class="small muted">The slider follows retained original witnesses: each literal span's first and last image and required event markers. Every analyzed reading remains in the raw result. Gaps between saved PNGs are shown, without interpolated images.</p>
<div id="frame-reading"></div><details><summary>Continuous original recording for context</summary><video id="original-video" controls preload="metadata" src="original.mov" style="width:100%;max-height:500px;background:#0b1112"></video><p class="small muted">The video follows the selected witness time where available. The unchanged PNG above supplies the exact comparison image; browser video seeking is contextual.</p></details></section>
<section class="panel" id="field-content"></section><section class="panel" id="source-content"></section></main></div>
<footer>Camera findings evaluate visible V1 presentation in the named replay and configuration. They do not establish audio behavior, RF detection, unexercised modes or internal firmware state. Times are host-input-to-capture observations, not a firmware-response deadline.</footer>
<script id="behavior-data" type="application/json">__RESULT_JSON__</script>
<script>
'use strict';
const report=JSON.parse(document.getElementById('behavior-data').textContent);
const events=report.events||[],fields=['counter_glyph','primary_frequency','active_bands','main_arrows','main_bars','secondary','muted_badge'];
const labels={counter_glyph:'Counter',primary_frequency:'Primary frequency',active_bands:'Active bands',main_arrows:'Main arrows',main_bars:'Strength bars',secondary:'Secondary cards',muted_badge:'MUTED badge',joint_state:'Shared display phase'};
const $=id=>document.getElementById(id),esc=v=>String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const json=v=>JSON.stringify(v,null,2),plain=v=>typeof v==='string'?v:json(v),ob=e=>e.observation||{},frameNo=w=>w?.frame_index??w?.video_frame_index;
const valueText=v=>{if(v===undefined||v===null)return 'Not established';if(Array.isArray(v))return v.length?v.map(valueText).join(' · '):'none';if(typeof v!=='object')return String(v);if(Array.isArray(v.allowed))return v.allowed.map(valueText).join(' or ');if(v.band||v.frequency)return [v.band,v.frequency,v.direction,Number.isFinite(v.bars)?v.bars+' bars':null].filter(x=>x!==null&&x!==undefined).join(' ');return JSON.stringify(v);};
const inputText=e=>{const rows=e.wire_rows||e.input?.rows||e.input;if(Array.isArray(rows))return rows.length?rows.map(r=>(r.priority?'Primary input: ':'Additional input: ')+[r.band,r.frequency,r.direction].filter(v=>v!==undefined&&v!==null).join(' ')+(Number.isFinite(r.bars)?' · row strength '+r.bars:'')).join('\n'):'No alert rows';return typeof rows==='string'?rows:rows?valueText(rows):typeof e.input_key==='string'?e.input_key:'Input details unavailable';};
const expectedTable=expected=>'<table><tbody>'+Object.entries(expected).map(([name,value])=>'<tr><td>'+esc(labels[name]||name)+'</td><td>'+esc(valueText(value))+'</td></tr>').join('')+'</tbody></table>';
const isFrame=w=>Number.isInteger(frameNo(w))&&frameNo(w)>=0;
const safeImage=w=>{const supplied=w?.image;if(typeof supplied==='string'&&!/^[a-z][a-z0-9+.-]*:/i.test(supplied)&&!/[\x00-\x1f\\]/.test(supplied)&&!supplied.startsWith('//'))return supplied;return isFrame(w)?'frames/'+String(frameNo(w)).padStart(6,'0')+'.png':null;};
const safeLink=url=>typeof url==='string'&&/^https?:\/\//.test(url)?url:null;
const safeLocalReport=path=>typeof path==='string'&&path.endsWith('.html')&&!path.startsWith('/')&&!/[:?#%\\\x00-\x1f]/.test(path)?path:null;
const anchor=e=>ob(e).input_anchor_ns,offset=(e,w)=>Number.isFinite(anchor(e))&&Number.isFinite(w?.capture_ns)?(w.capture_ns-anchor(e))/1e6:null;
const ms=n=>Number.isFinite(n)?(n>=0?'+':'')+n.toFixed(3)+' ms':'time unavailable';
const witness=(e,w,label)=>isFrame(w)?'<button class="witness" data-frame="'+frameNo(w)+'">'+esc(label||'Frame '+frameNo(w))+' · '+esc(ms(offset(e,w)))+'</button>':'<span class="muted">No original witness</span>';
const literal=w=>{const r=w?.observed??w;if(r===undefined||r===null)return 'No reading';if(typeof r!=='object')return String(r);if('state'in r){const value=r.value===undefined||r.value===null?'':valueText(r.value);return r.state+ (value?' · '+value:'')+(r.reason?'\n'+r.reason:'');}return valueText(r);};
const unresolved=e=>Object.values(ob(e).fields||{}).reduce((n,f)=>n+(f.counts?.unresolved_frames||0),0);
const persistenceCases=e=>(report.persistence?.cases||[]).filter(c=>c.event_id===e.event_id);
const targetSeen=e=>{const cases=persistenceCases(e);return cases.length?cases.every(c=>c.stage_order_observed&&!c.missing_stages?.length&&c.complete_recorded_frame_coverage):ob(e).target_observed;};
const findingList=e=>[...(e.findings||[]),...persistenceCases(e).flatMap(c=>c.findings.map(f=>({...f,field:c.kind.includes('secondary')?'secondary':'primary_frequency',expected:c.required_stages,observed:f.observed||f.stage,rule_ids:['persistence_and_clear','retired_card']})))];
const isPersistent=f=>['ending_difference','departure_after_target'].includes(f.kind)||/persist|held|retained|stale/i.test(f.kind||'');
let activeIndex=0,activeFrames=[],framePosition=0,frameMap=new Map();
for(const sample of report.samples_index||[])if(isFrame(sample))frameMap.set(frameNo(sample),sample);
function identity(){const evidence=report.evidence||{},runtime=evidence.runtime_identity||{},source=evidence.tooling_source||{};
 const sha=runtime.git_sha||runtime.firmware_commit||runtime.commit||'unidentified',image=runtime.image_id||runtime.elf_sha256||runtime.image_sha256||'unidentified';
 $('identity').innerHTML='<span>Recorded firmware <strong>'+esc(sha)+'</strong></span><span>Image <strong>'+esc(image)+'</strong></span>'+(runtime.boot_id!==undefined?'<span>Boot <strong>'+esc(runtime.boot_id)+'</strong></span>':'');
 const target=report.summary?.targets_observed??events.filter(e=>ob(e).target_observed).length,persistent=report.summary?.events_with_findings??events.filter(e=>findingList(e).some(isPersistent)).length,unknown=report.summary?.unresolved_field_observations??events.reduce((n,e)=>n+unresolved(e),0);
 $('stats').innerHTML=[[events.length,'authored input events'],[report.persistence?(report.persistence.summary?.complete_sequences??0)+' / '+(report.persistence.summary?.cases??0):target+' / '+events.length,report.persistence?'persistence sequences observed':'complete target observed'],[report.persistence?events.filter(e=>findingList(e).length).length:persistent,'events with contrary content'],[unknown.toLocaleString(),'unresolved field readings retained']].map(([n,label])=>'<div class="stat"><b>'+esc(n)+'</b><span>'+esc(label)+'</span></div>').join('');
 const read=report.summary?.read_frames,available=report.summary?.available_frames;
 $('coverage-summary').textContent=Number.isFinite(read)&&Number.isFinite(available)?read.toLocaleString()+' / '+available.toLocaleString()+' recorded event frames analyzed. '+(report.summary?.unresolved_frames??'Uncounted')+' frames contain unresolved readings.':'';
 const outcome=report.result||report.status||report.summary?.result;
 $('overall-result').textContent=({DIFFERENCES_FOUND:'Physical display differences were found. Inspect the event findings and original witnesses below.',MEASUREMENT_INCOMPLETE:'Measurement is incomplete. Missing targets, evidence or coverage are identified below.',NO_DIFFERENCES_OBSERVED:'No differences were observed in the evaluated scope. Unreadable transition images remain explicit.'})[outcome]||'';
 const scope=report.scope||report.behavior_contract?.scope||'Visible V1 display behavior in the controlled replay.';
 const scopeHTML=typeof scope==='string'?'<p>'+esc(scope)+'</p>':'<p>'+esc(scope.meaning||'')+'</p>'+(scope.not_measured?'<p><strong>Not measured:</strong> '+esc(valueText(scope.not_measured))+'</p>':'')+(scope.timing?'<p><strong>Timing:</strong> '+esc(valueText(scope.timing))+'</p>':'');
 $('method').innerHTML=scopeHTML+'<p>Tooling and firmware identities are separate. A target seen once is not proof that the rest of the event remained correct.</p><details><summary>Recorded identity</summary><pre>'+esc(json(runtime))+'</pre></details><details><summary>Bound settings</summary><pre>'+esc(json(evidence.configuration||{}))+'</pre></details><details><summary>Reader and tooling identity</summary><pre>'+esc(json({reader:report.reader_method,tooling:source}))+'</pre></details><details><summary>Collection and analysis coverage</summary><pre>'+esc(json(report.coverage||report.summary||{}))+'</pre></details>';
 if(report.errors?.length)$('global-errors').innerHTML='<div class="notice"><strong>Analysis limitations</strong><pre>'+esc(json(report.errors))+'</pre></div>';
}
function renderIndex(){const query=$('search').value.toLowerCase(),filter=$('filter').value;
 const visible=events.map((e,i)=>({e,i})).filter(({e})=>{const match=!query||JSON.stringify([e.event_id,e.input_key,e.input,e.wire_rows,e.target,e.findings]).toLowerCase().includes(query);return match&&(filter==='all'||filter==='findings'&&findingList(e).length||filter==='unknown'&&unresolved(e)||filter==='missing'&&!targetSeen(e)||filter==='observed'&&targetSeen(e));});
 $('list-count').textContent=visible.length+' of '+events.length+' events';
 $('event-list').innerHTML=visible.map(({e,i})=>{const finds=findingList(e).length,seen=targetSeen(e);return '<button class="event-button '+(i===activeIndex?'active':'')+'" data-event="'+i+'"><b>'+esc(e.event_id)+'</b><span class="tag '+(finds?'bad':seen?'':'warn')+'">'+esc(finds?finds+' finding'+(finds===1?'':'s'):seen?(persistenceCases(e).length?'Sequence observed':'Target observed'):(persistenceCases(e).length?'Sequence incomplete':'Target not seen'))+'</span><span>'+esc(inputText(e).slice(0,130))+'</span></button>';}).join('')||'<div class="empty">No events match this filter.</div>';
}
function rulesFor(event){const rules=report.behavior_contract?.rules||{},ids=new Set(findingList(event).flatMap(f=>f.rule_ids||[]));
 if(!ids.size)for(const name of fields)for(const id of report.behavior_contract?.field_rule_ids?.[name]||[])ids.add(id);
 return [...ids].filter(id=>rules[id]).map(id=>({id,...rules[id]}));}
function persistenceHTML(e){const cases=persistenceCases(e);if(!cases.length)return '';
 return '<h3>Configured persistence sequence</h3><p>'+esc(report.persistence.configured_seconds)+' second setting. Stages below are measured from literal images; offsets do not locate DUT receipt or exact timer expiry.</p>'+cases.map(c=>'<h3>'+esc(c.kind.replace(/_/g,' '))+'</h3><p>Required order: '+esc(c.required_stages.join(' → '))+'. '+(c.stage_order_observed?'Observed in order.':'Required sequence not fully observed.')+'</p>'+(c.missing_stages?.length?'<p class="notice">Missing observations: '+esc(c.missing_stages.join(', '))+'</p>':'')+'<div class="table-wrap"><table><thead><tr><th>Observed stage</th><th>Frames</th><th>First original</th><th>Last original</th></tr></thead><tbody>'+Object.entries(c.stages||{}).map(([name,stage])=>'<tr><td>'+esc(name.replace(/_/g,' '))+'</td><td>'+esc(stage.frames)+'</td><td>'+witness(e,stage.first)+'</td><td>'+witness(e,stage.last)+'</td></tr>').join('')+'</tbody></table></div>').join('')+'<p class="small muted">The fixed-target field table below is supporting detail. Positive persistence is evaluated as an ordered sequence, because its idle presentation changes while the input stays empty.</p>';
}
function findingHTML(e,f){return '<article class="finding '+(isPersistent(f)?'':'neutral')+'"><h3>'+esc(labels[f.field]||f.field||'Display')+' · '+esc((f.kind||'Observation').replace(/_/g,' '))+'</h3><p>'+esc(f.reason||'')+'</p><div class="facts"><div><b>Expected from input / source</b><pre class="literal">'+esc(valueText(f.expected))+'</pre></div><div><b>Observed in the physical image</b><pre class="literal">'+esc(literal(f.observed))+'</pre></div></div>'+(f.observation_detail?'<details><summary>Independent partial-content reading</summary><pre>'+esc(valueText(f.observation_detail))+'</pre></details>':'')+'<div>'+witness(e,f.first,'First witness')+' '+witness(e,f.last,'Last witness')+'</div></article>';}
function phaseHTML(e){const p=e.phase_observation;if(!p||!Array.isArray(p.required_phase_ids)||p.required_phase_ids.length<2)return '';
 const required=p.required_phase_ids,observed=p.observed_phase_ids||[],alternations=p.alternation_count??p.alternations?.length??0;
 const rows=required.map(id=>'<tr><td>'+esc(id)+'</td><td>'+esc(p.phase_counts?.[id]||0)+'</td><td>'+witness(e,p.first_observations?.[id],'First')+'</td><td>'+witness(e,p.last_observations?.[id],'Last')+'</td></tr>').join('');
 const spans=(p.contiguous_phase_spans||[]).map(s=>'<tr><td>'+esc(s.phase_id)+'</td><td>'+witness(e,s.first,'First')+' '+witness(e,s.last,'Last')+'</td><td>'+esc(s.frame_count)+'</td><td>'+esc(Number.isFinite(s.duration_ms)?s.duration_ms.toFixed(3)+' ms':'unavailable')+'</td></tr>').join('');
 return '<h3>Visible blink function</h3><p>'+observed.length+' / '+required.length+' required joint phases observed; '+alternations+' phase alternations between adjacent readable captures; '+esc(p.unresolved_frames||0)+' unresolved joint-phase frames. '+(Number.isFinite(p.source_toggle_ms)?'Recorded source toggles every '+esc(p.source_toggle_ms)+' ms.':'Source toggle cadence is unverified.')+'</p><details><summary>Required phases and contiguous observations</summary><p>Required: '+esc(required.join(', '))+'. Observed: '+esc(observed.join(', ')||'none')+'.</p><div class="table-wrap"><table><thead><tr><th>Required phase</th><th>Readable frames</th><th>First original</th><th>Last original</th></tr></thead><tbody>'+rows+'</tbody></table></div>'+(spans?'<h3>Contiguous readable phase observations</h3><div class="table-wrap"><table><thead><tr><th>Phase</th><th>Original witnesses</th><th>Frames</th><th>Capture-marker span</th></tr></thead><tbody>'+spans+'</tbody></table></div>':'<p>No contiguous readable phase observations.</p>')+'<p class="small muted">Unknown joint readings and capture gaps break the intervals above. Blink cadence describes source behavior; it supplies no input-to-display deadline.</p></details>';}
function selectEvent(index,updateHash=true){if(!events[index])return;activeIndex=index;const e=events[index],o=ob(e),first=o.first_target_observation;renderIndex();
 if(updateHash){const hash='#event='+encodeURIComponent(e.event_id);if(location.hash!==hash)location.hash=hash;}
 const start=Number.isFinite(e.start_ns)?e.start_ns:anchor(e),end=e.end_ns??e.coverage?.end_ns??o.coverage?.end_ns;
 activeFrames=(report.samples_index||[]).filter(s=>s.event_id===e.event_id||(!s.event_id&&Number.isFinite(start)&&Number.isFinite(end)&&s.capture_ns>=start&&s.capture_ns<end));
 const seenFrames=new Set(activeFrames.map(frameNo));
 for(const w of [first,...findingList(e).flatMap(f=>[f.first,f.last]),...Object.values(e.phase_observation?.first_observations||{}),...Object.values(e.phase_observation?.last_observations||{}),...(e.phase_observation?.contiguous_phase_spans||[]).flatMap(s=>[s.first,s.last]),...Object.values(o.fields||{}).flatMap(f=>[f.first_target_observation,f.latest_contrary_observation,f.end_state?.last_observation,...(f.unresolved_intervals||[]).flatMap(u=>[u.first,u.last])])])if(isFrame(w)&&!seenFrames.has(frameNo(w))){activeFrames.push(w);seenFrames.add(frameNo(w));frameMap.set(frameNo(w),w);}
 activeFrames.sort((a,b)=>frameNo(a)-frameNo(b));
 const expected=e.target?.fields||e.target||{},count=findingList(e).length;
 $('event-content').innerHTML='<div class="event-heading"><h2>'+esc(e.event_id)+'</h2><span class="tag '+(count?'bad':targetSeen(e)?'':'warn')+'">'+esc(count?count+' contrary-content finding'+(count===1?'':'s'):targetSeen(e)?(persistenceCases(e).length?'Required sequence observed':'Complete target observed'):(persistenceCases(e).length?'Sequence incomplete':'Complete target not observed'))+'</span></div><div class="facts"><div><h3>Controlled input</h3><pre class="reading literal">'+esc(inputText(e))+'</pre><details><summary>Accepted input details</summary><pre>'+esc(plain(e.input||e.wire_rows||e.input_key||{}))+'</pre></details></div><div><h3>Expected display</h3>'+expectedTable(expected)+'</div></div><h3>Observed response</h3><p>'+(persistenceCases(e).length?'The required display stages and original witnesses are shown below.':first?'The first complete target was read at '+witness(e,first)+'.':'No complete target was read in this event. Individual matching, differing and unreadable fields remain below.')+'</p>'+(o.target_already_correct_in_preceding_observation?'<p class="notice">The target also matched the preceding image. This does not establish a new visual response to this input.</p>':'')+'<p class="small muted">Offsets use complete host input as zero. Partial transitions are retained as measurements, not automatically treated as firmware faults.</p>'+persistenceHTML(e)+phaseHTML(e)+findingList(e).map(f=>findingHTML(e,f)).join('')+'<details><summary>Event coverage and ending boundary</summary><pre>'+esc(json(e.coverage||o.coverage||{}))+'</pre></details>';
 renderFields(e);renderSources(e);const selected=first&&activeFrames.findIndex(s=>frameNo(s)===frameNo(first));showFrame(Number.isInteger(selected)&&selected>=0?selected:0);if(updateHash)$('event-content').scrollIntoView({block:'start'});}
function renderFields(e){const o=ob(e);
 const rows=fields.filter(name=>o.fields?.[name]).map(name=>{const f=o.fields[name],end=f.end_state||{},unknownEnd=(end.unresolved_suffix||[]).reduce((n,s)=>n+s.frame_count,0),counts=f.counts||{},bracket=f.first_target_capture_bracket;
 const completion=f.first_target_observation?witness(e,f.first_target_observation,'First target'):'Not observed';
 let bounds=bracket?'<details><summary>Acquisition markers</summary><p>Preceding reading: '+witness(e,bracket.preceding_observation)+'</p><p>Last definite difference: '+witness(e,bracket.last_definite_difference)+'</p><p>'+esc(bracket.intervening_unresolved_frames||0)+' intervening unresolved frames; '+esc((bracket.recorded_frame_gaps||[]).length)+' retained sampling gaps.</p><p class="muted">These bound recorded observations; they do not establish an exact optical settling time.</p></details>':'';
 const suffix=unknownEnd?'<p class="small"><span class="tag warn">'+unknownEnd+' unreadable ending frames</span></p>':'';
 return '<tr><td>'+esc(labels[name])+'</td><td>'+completion+bounds+'</td><td><pre class="literal">'+esc(literal(end.last_observation))+'</pre>'+witness(e,end.last_observation,'Last image')+suffix+(unknownEnd&&end.last_definite_observation?'<p class="small">Last definite reading: '+esc(literal(end.last_definite_observation))+' '+witness(e,end.last_definite_observation)+'</p>':'')+'</td><td>'+esc(counts.matching_frames||0)+' match<br>'+esc(counts.different_frames||0)+' different<br>'+esc(counts.unresolved_frames||0)+' unresolved'+(f.post_target_different_frames?'<p class="tag bad">'+esc(f.post_target_different_frames)+' different after first target</p>':'')+(f.latest_contrary_observation?'<p>'+witness(e,f.latest_contrary_observation,'Latest difference')+'</p>':'')+'</td></tr>';}).join('');
 const intervals=fields.filter(name=>(o.fields?.[name]?.unresolved_intervals||[]).length||(o.fields?.[name]?.difference_intervals||[]).length).map(name=>{const f=o.fields[name];return '<details><summary>'+esc(labels[name])+': differing and unresolved intervals</summary>'+['difference_intervals','unresolved_intervals'].map(key=>{const items=f[key]||[];return items.length?'<h3>'+esc(key==='difference_intervals'?'Literal differences':'Unreadable observations')+'</h3><div class="table-wrap"><table><thead><tr><th>First / last original</th><th>Literal reading</th><th>Frames</th></tr></thead><tbody>'+items.map(s=>'<tr><td>'+witness(e,s.first,'First')+' '+witness(e,s.last,'Last')+'</td><td><pre class="literal">'+esc(literal(s.first))+'</pre></td><td>'+esc(s.frame_count)+'</td></tr>').join('')+'</tbody></table></div>':'';}).join('')+'</details>';}).join('');
 $('field-content').innerHTML='<h2>What each field did</h2><p class="small muted">A first matching value and a later incorrect value are both retained. Legal shared blink phases remain matching states. Frame counts are field observations, not counts of firmware bugs.</p><div class="table-wrap"><table><thead><tr><th>Field</th><th>First target / bounds</th><th>Ending observation</th><th>Recorded field readings</th></tr></thead><tbody>'+rows+'</tbody></table></div>'+intervals;
}
function renderSources(e){const contract=report.behavior_contract||{},rules=rulesFor(e);
 $('source-content').innerHTML='<h2>Owning source and repair direction</h2><p class="small muted">Source correspondence: '+esc(contract.status||'unavailable')+'. The code explanation is distinct from a proved physical cause.</p>'+rules.map(rule=>'<details class="rule" '+(findingList(e).some(f=>(f.rule_ids||[]).includes(rule.id))?'open':'')+'><summary>'+esc(rule.id.replace(/_/g,' '))+' · '+esc(rule.status||'')+'</summary><p>'+esc(rule.statement)+'</p><p><strong>Repair / investigation direction:</strong> '+esc(rule.repair_direction)+'</p><div class="source-links">'+(rule.locations||[]).map(loc=>{const url=safeLink(loc.url);return url?'<a target="_blank" rel="noopener" href="'+esc(url)+'">'+esc(loc.path)+(loc.line_start?':'+loc.line_start:'')+' ↗</a>':esc(loc.path);}).join('')+'</div><details><summary>Recorded source excerpts</summary>'+(rule.locations||[]).filter(l=>l.excerpt).map(l=>'<p class="small">'+esc(l.path)+'</p><pre>'+esc(l.excerpt)+'</pre>').join('')+'</details></details>').join('');
}
function showFrame(position){const e=events[activeIndex];framePosition=Math.max(0,Math.min(position,activeFrames.length-1));const sample=activeFrames[framePosition],url=safeImage(sample);
 $('frame-slider').max=String(Math.max(0,activeFrames.length-1));$('frame-slider').value=String(framePosition);$('frame-slider').disabled=!activeFrames.length;$('prev-frame').disabled=!activeFrames.length||framePosition===0;$('next-frame').disabled=!activeFrames.length||framePosition===activeFrames.length-1;
 if(url){$('frame-image').src=url;$('frame-image').alt='Unchanged original frame '+frameNo(sample)+' for '+e.event_id;$('frame-image').hidden=false;$('original-link').href=url;$('original-link').hidden=false;}else{$('frame-image').removeAttribute('src');$('frame-image').hidden=true;$('original-link').removeAttribute('href');$('original-link').hidden=true;}
 const prev=activeFrames[framePosition-1],gap=prev&&frameNo(sample)-frameNo(prev)>1?' · '+(frameNo(sample)-frameNo(prev)-1)+' intervening recorded frame positions not retained here':'';
 $('viewer-context').textContent=e?e.event_id+' · '+activeFrames.length+' retained original witnesses':'';
 $('frame-label').textContent=sample?'Frame '+frameNo(sample)+' · '+ms(offset(e,sample))+' from complete host input · '+(framePosition+1)+' / '+activeFrames.length+gap:'No original images were supplied for this event.';
 const span=sample&&(e.observation_spans||[]).find(s=>frameNo(s.first)<=frameNo(sample)&&frameNo(s.last)>=frameNo(sample));
 $('frame-reading').innerHTML=span?'<h3>This original image: independent reading versus expected content</h3><div class="table-wrap"><table><thead><tr><th>Field</th><th>Observed pixels</th><th>Expected from input</th><th>Comparison</th></tr></thead><tbody>'+fields.map(name=>'<tr><td>'+esc(labels[name])+'</td><td><pre class="literal">'+esc(literal(span.observed?.[name]))+'</pre></td><td><pre class="literal">'+esc(valueText(e.target?.fields?.[name]??e.target?.[name]))+'</pre></td><td>'+esc(span.judgment?.fields?.[name]||'Not compared')+'</td></tr>').join('')+'</tbody></table></div><p class="small muted">Shared display phase: '+esc(span.judgment?.joint_state||'not evaluated')+'.</p>':'<p class="small muted">No per-frame literal reading was supplied for this witness.</p>';
 const video=$('original-video');if(Number.isFinite(sample?.video_seconds)){const seek=()=>{try{video.currentTime=sample.video_seconds;}catch(error){}};if(video.readyState>=1)seek();else video.addEventListener('loadedmetadata',seek,{once:true});}
}
function jumpFrame(number){let position=activeFrames.findIndex(s=>frameNo(s)===number);if(position<0&&frameMap.has(number)){activeFrames.push(frameMap.get(number));activeFrames.sort((a,b)=>frameNo(a)-frameNo(b));position=activeFrames.findIndex(s=>frameNo(s)===number);}if(position>=0){showFrame(position);$('viewer').scrollIntoView({behavior:'smooth',block:'start'});}}
function renderComparison(){const comparison=report.comparison;if(!comparison)return;const changes=(comparison.events||[]).filter(e=>e.changed),baselinePath=safeLocalReport(comparison.baseline_report);
 const identity=run=>{const r=run?.runtime_identity||{};return 'Firmware '+(r.git_sha||'unidentified')+' · image '+(r.image_id||'unidentified');};
 const baselineEventLink=(id,label)=>baselinePath?'<a href="'+esc(baselinePath+'#event='+encodeURIComponent(id))+'" target="_blank" rel="noopener">'+esc(label)+'</a>':esc(label);
 const fieldUnknown=run=>Object.values(run?.fields||{}).reduce((v,f)=>({frames:v.frames+(f.counts?.unresolved_frames||0),suffix:v.suffix+(f.unresolved_suffix_frames||0)}),{frames:0,suffix:0});
 const findingWitness=(finding,e,run)=>{const w=finding.last;if(!isFrame(w))return '';if(run==='current'){const i=events.findIndex(item=>item.event_id===e.event_id);return i<0?'':'<button class="witness" data-compare-event="'+i+'" data-frame="'+frameNo(w)+'">Current original '+frameNo(w)+' · '+esc(ms(finding.last_ms))+'</button>';}
   const image=safeImage(w),folder=baselinePath?baselinePath.slice(0,baselinePath.lastIndexOf('/')+1):null;return folder!==null&&image&&!image.startsWith('/')&&!image.split('/').includes('..')?'<a class="witness" href="'+esc(folder+image)+'" target="_blank" rel="noopener">Baseline original '+frameNo(w)+' · '+esc(ms(finding.last_ms))+'</a>':baselineEventLink(e.baseline_event_id,'Baseline event');};
 const findings=(items,e,run)=>items.length?items.map(f=>'<p><strong>'+esc(labels[f.field]||f.field)+'</strong>: '+esc(literal(f.observed))+'<br>'+findingWitness(f,e,run)+'</p>').join(''):'<span class="muted">none</span>';
 const rows=changes.map(e=>{const i=events.findIndex(item=>item.event_id===e.event_id),a=e.appearance||{},oldUnknown=fieldUnknown(e.baseline),newUnknown=fieldUnknown(e.current);return '<tr><td>'+(i<0?esc(e.event_id):'<button data-event="'+i+'">'+esc(e.event_id)+'</button>')+'<p>'+baselineEventLink(e.baseline_event_id,'Baseline event')+'</p></td><td>Baseline: '+esc(ms(a.baseline_ms))+'<br>Current: '+esc(ms(a.current_ms))+(Number.isFinite(a.difference_ms)?'<p>Measured change: '+esc(ms(a.difference_ms))+'</p>':'')+'</td><td>'+findings(e.newly_observed_findings||[],e,'current')+'</td><td>'+findings(e.previously_observed_findings_absent||[],e,'baseline')+'</td><td>Unresolved field readings: '+oldUnknown.frames+' → '+newUnknown.frames+'<br>Ending unresolved: '+oldUnknown.suffix+' → '+newUnknown.suffix+(e.coverage_changed?'<p class="tag warn">Coverage changed</p>':'')+'<details><summary>Field and coverage changes</summary><pre>'+esc(json({baseline:{fields:e.baseline?.fields,coverage:e.baseline?.coverage},current:{fields:e.current?.fields,coverage:e.current?.coverage}}))+'</pre></details></td></tr>';}).join('');
 $('comparison').innerHTML='<section class="panel"><h2>Firmware comparison</h2><div class="facts"><p><strong>Baseline</strong><br>'+esc(identity(comparison.baseline))+'</p><p><strong>Current</strong><br>'+esc(identity(comparison.current))+'</p></div><p>'+esc(comparison.basis||'Like-for-like event observations.')+'</p>'+(comparison.reasons?.length?'<div class="notice">'+esc(comparison.reasons.join('; '))+'</div>':'')+(comparison.compatible?'<p>'+changes.length+' changed event observations / '+(comparison.summary?.compared_events??comparison.events?.length??0)+' comparable events. Timing differences are measurements; absent findings do not establish a repair.</p>':'')+(rows?'<div class="table-wrap"><table><thead><tr><th>Event</th><th>First complete target</th><th>Newly observed findings</th><th>Previously seen, absent here</th><th>Uncertainty and coverage</th></tr></thead><tbody>'+rows+'</tbody></table></div>':'<p class="muted">'+(comparison.compatible?'No changed observations.':'These recordings cannot be compared under the same input and measurement rules.')+'</p>')+'</section>';
}
document.addEventListener('click',event=>{const compareButton=event.target.closest('[data-compare-event]');if(compareButton){selectEvent(Number(compareButton.dataset.compareEvent));jumpFrame(Number(compareButton.dataset.frame));return;}const eventButton=event.target.closest('[data-event]');if(eventButton){selectEvent(Number(eventButton.dataset.event));return;}const frameButton=event.target.closest('[data-frame]');if(frameButton)jumpFrame(Number(frameButton.dataset.frame));});
$('search').addEventListener('input',renderIndex);$('filter').addEventListener('change',renderIndex);$('frame-slider').addEventListener('input',event=>showFrame(Number(event.target.value)));$('prev-frame').addEventListener('click',()=>showFrame(framePosition-1));$('next-frame').addEventListener('click',()=>showFrame(framePosition+1));
$('frame-image').addEventListener('error',()=>{$('frame-label').textContent+=' · Original image could not be loaded; no replacement is displayed.';});
function indexFromHash(){const id=new URLSearchParams(location.hash.slice(1)).get('event');return events.findIndex(e=>e.event_id===id);}
window.addEventListener('hashchange',()=>{const i=indexFromHash();if(i>=0&&i!==activeIndex){selectEvent(i,false);$('event-content').scrollIntoView({block:'start'});}});
identity();renderComparison();renderIndex();if(events.length){const requested=indexFromHash();selectEvent(requested>=0?requested:0,false);if(requested>=0)$('event-content').scrollIntoView({block:'start'});}else{$('event-content').innerHTML='<h2>No event observations</h2><p>The result contains no evaluated input events. See analysis limitations above.</p>';$('viewer').hidden=true;$('field-content').hidden=true;$('source-content').hidden=true;}
</script></body></html>'''
