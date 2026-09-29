(* Reads a command stream in the canonical text format and prints every event the reference model emits, one
   per line. A CONFIG line starts a fresh book: the first line of every stream, and again wherever the producer
   rebuilt its book (exsim_l3replay does after a gap in the exchange feed). scripts/ocaml_diff.sh compares this
   output with the C++ engine's for the same stream (tools/exsim_difffeed.cpp): they must be identical. *)

let () =
  let ic = if Array.length Sys.argv > 1 then open_in Sys.argv.(1) else stdin in
  let book = ref None in
  let commands = ref 0 in
  let out = Buffer.create (1 lsl 20) in
  let check_invariants () =
    match !book with
    | Some b when not (Exsim_ref.invariants b) -> prerr_endline "invariant violated"; exit 1
    | _ -> ()
  in
  (try
     while true do
       let line = input_line ic in
       match Exsim_ref.config_of_string line with
       | Some config ->
           check_invariants ();
           book := Some (Exsim_ref.create config)
       | None -> (
           match (Exsim_ref.command_of_string line, !book) with
           | Some cmd, Some b ->
               incr commands;
               let b, events = Exsim_ref.apply b cmd in
               book := Some b;
               List.iter (fun e -> Buffer.add_string out (Exsim_ref.string_of_event e); Buffer.add_char out '\n') events;
               if Buffer.length out > 1 lsl 20 then (print_string (Buffer.contents out); Buffer.clear out)
           | Some _, None -> failwith "a command before the first CONFIG line"
           | None, _ -> ())
     done
   with End_of_file -> ());
  print_string (Buffer.contents out);
  check_invariants ();
  Printf.eprintf "%d commands\n" !commands
