(* Reads a command stream in the canonical text format (first line CONFIG, then one command per line) and
   prints every event the reference model emits, one per line. scripts/ocaml_diff.sh compares this output
   with the C++ engine's output for the same stream (tools/exsim_difffeed.cpp): they must be identical. *)

let () =
  let ic = if Array.length Sys.argv > 1 then open_in Sys.argv.(1) else stdin in
  let config =
    match Exsim_ref.config_of_string (input_line ic) with
    | Some c -> c
    | None -> failwith "first line must be: CONFIG min_price num_levels max_orders stp"
  in
  let book = ref (Exsim_ref.create config) in
  let out = Buffer.create (1 lsl 20) in
  (try
     while true do
       match Exsim_ref.command_of_string (input_line ic) with
       | None -> ()
       | Some cmd ->
           let b, events = Exsim_ref.apply !book cmd in
           book := b;
           List.iter (fun e -> Buffer.add_string out (Exsim_ref.string_of_event e); Buffer.add_char out '\n') events;
           if Buffer.length out > 1 lsl 20 then (print_string (Buffer.contents out); Buffer.clear out)
     done
   with End_of_file -> ());
  print_string (Buffer.contents out);
  if not (Exsim_ref.invariants !book) then (prerr_endline "invariant violated"; exit 1)
