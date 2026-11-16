package com.Bilibili_Innocent_Lab.xposedmodule.hook.feature.interpolation

import java.io.File
import java.util.Locale

/* Executes the same fixed JSONL fixture against the unmodified BIL timeline.
 * This deliberately supports only the fixture's flat string/integer fields. */
fun main(args: Array<String>) {
    require(args.size == 1)
    Locale.setDefault(Locale.ROOT)
    val token = Regex("\"([a-z_]+)\":(\"[a-z_]+\"|-?[0-9]+)")
    var timeline: FrameTimeline? = null
    var lease: FrameTimeline.Lease? = null
    var pending: FrameTimeline.Proposal? = null
    var generation = 0L
    val reasons = FrameTimeline.Reason.entries.withIndex().associate { it.value to it.index + 1 }
    for (line in File(args[0]).readLines()) {
        val fields = token.findAll(line).associate { it.groupValues[1] to it.groupValues[2].trim('"') }
        fun n(key: String) = fields.getValue(key).toLong()
        fun discarded(ids: List<Long>) = ids.joinToString(",")
        val seq=n("seq"); val op=fields.getValue("op")
        when(op) {
            "config" -> { timeline=FrameTimeline(n("target_fps").toInt(),maxClockAgeNs=n("max_clock_age_ns"),
                presentationDelayNs=n("presentation_delay_ns"),maxLeadNs=n("max_lead_ns").takeIf { it != 0L },
                minLeadNs=n("min_lead_ns"),originalToleranceUs=n("original_tolerance_us"),
                phaseLock=n("phase_lock") != 0L,maxClockUncertaintyUs=n("max_clock_uncertainty_us"),frameWindow=n("frame_window").toInt())
                println("$seq|config") }
            "begin" -> { val begin=timeline!!.begin(FrameTimeline.Epoch(n("media"),n("decoder"),n("surface"),n("format"),n("discontinuity")))
                lease=begin.lease; generation++; pending=null; println("$seq|begin|$generation|${discarded(begin.discardedIds)}") }
            "clock" -> { val v=timeline!!.updateClock(lease!!,FrameTimeline.ClockSnapshot(n("media_us"),n("monotonic_ns"),
                n("speed_milli")/1000.0,n("paused") != 0L,n("discontinuity"),n("uncertainty_us")))
                val state=when(v.state){FrameTimeline.ClockState.READY->1;FrameTimeline.ClockState.PAUSED->2;
                    FrameTimeline.ClockState.STALE_SNAPSHOT->3;FrameTimeline.ClockState.RESTART_REQUIRED->4;else->error("unexpected clock state")}
                println("$seq|clock|$state|${discarded(v.discardedIds)}") }
            "offer" -> { val v=timeline!!.offer(lease!!,FrameTimeline.Frame(n("frame_id"),n("pts_us")))
                println("$seq|offer|${if(v.accepted)1 else 0}|${discarded(v.discardedIds)}") }
            "plan" -> when(val v=timeline!!.plan(lease!!,n("now_ns"))) {
                is FrameTimeline.Decision.Ready -> { val p=v.proposal; pending=p
                    println(String.format(Locale.ROOT,"%d|ready|%d|%d|%d|%d|%.15f|%d",seq,p.dueNs,p.mediaUs,p.before.id,p.after.id,p.alpha,if(p.isOriginal)1 else 2)) }
                is FrameTimeline.Decision.Waiting -> println("$seq|wait|${reasons.getValue(v.reason)}|${v.dueNs?:0}|${v.mediaUs?:0}") }
            "commit" -> { val accepted=pending?.let { timeline!!.commit(it,n("now_ns")) } ?: false
                pending=null; println("$seq|commit|${if(accepted)0 else -1}") }
            "close" -> { println("$seq|close|${discarded(timeline!!.close(lease!!))}"); lease=null; pending=null }
            else -> error("unsupported fixture operation $op")
        }
    }
}
