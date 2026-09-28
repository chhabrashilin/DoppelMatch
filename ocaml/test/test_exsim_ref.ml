(* Expect tests: each scenario's full event stream is pinned in the source, so a behavioural change shows up
   as a readable diff (run `dune runtest`, then `dune promote` to accept an intended change). *)

open Exsim_ref

let config ?(stp = Stp_none) () = { min_price = 1000; num_levels = 1024; max_orders = 64; default_stp = stp }

let limit ?(tif = Day) ?(post_only = false) ?stp ?(owner = 1) id side price qty =
  New { id; side; price; qty; ord_type = Limit; tif; post_only; stp; owner }

let market ?(owner = 1) id side qty = New { id; side; price = 0; qty; ord_type = Market; tif = Day; post_only = false; stp = None; owner }

(* Applies the commands in order and prints every event. *)
let run ?(config = config ()) cmds =
  let _book =
    List.fold_left
      (fun book cmd ->
        let book, events = apply book cmd in
        List.iter (fun e -> print_endline (string_of_event e)) events;
        book)
      (create config) cmds
  in
  ()

let%expect_test "price then time priority; trades at the maker's price" =
  run [ limit 1 Sell 1505 5; limit 2 Sell 1505 5; limit 3 Sell 1504 5; limit 4 Buy 1505 12 ];
  [%expect {|
    A 1 1 1505 5
    A 2 1 1505 5
    A 3 1 1504 5
    A 4 0 1505 12
    T 4 3 0 1504 5 0
    T 4 1 0 1505 5 0
    T 4 2 0 1505 2 3 |}]

let%expect_test "IOC remainder expires; FOK is all or nothing" =
  run [ limit 1 Sell 1505 5; limit ~tif:Ioc 2 Buy 1505 8; limit 3 Sell 1506 5; limit ~tif:Fok 4 Buy 1506 6; limit ~tif:Fok 5 Buy 1506 5 ];
  [%expect {|
    A 1 1 1505 5
    A 2 0 1505 8
    T 2 1 0 1505 5 0
    C 2 0 1505 3 10
    A 3 1 1506 5
    A 4 0 1506 6
    C 4 0 1506 6 11
    A 5 0 1506 5
    T 5 3 0 1506 5 0 |}]

let%expect_test "modify: a size decrease keeps priority, a reprice loses it and may trade" =
  run
    [ limit 1 Buy 1500 10; limit 2 Buy 1500 10; Modify_order { id = 1; price = 1500; qty = 5 };
      limit 3 Sell 1500 5; limit 4 Sell 1502 5; Modify_order { id = 2; price = 1502; qty = 10 } ];
  [%expect {|
    A 1 0 1500 10
    A 2 0 1500 10
    M 1 0 1500 5 0
    A 3 1 1500 5
    T 3 1 1 1500 5 0
    A 4 1 1502 5
    M 2 0 1502 10 0
    T 2 4 0 1502 5 0 |}]

let%expect_test "market orders sweep levels, then expire" =
  run [ limit 1 Sell 1505 5; limit 2 Sell 1600 5; market 3 Buy 20; market 4 Sell 7 ];
  [%expect {|
    A 1 1 1505 5
    A 2 1 1600 5
    A 3 0 0 20
    T 3 1 0 1505 5 0
    T 3 2 0 1600 5 0
    C 3 0 0 10 10
    A 4 1 0 7
    C 4 1 0 7 10 |}]

let%expect_test "validation order and rejections" =
  run
    [ limit 0 Buy 1500 1; limit 1 Buy 1500 0; limit 1 Buy 999 1; limit 1 Buy 1000 1; limit 1 Sell 1500 1;
      Cancel_order { id = 9 }; limit 2 Sell 1001 1; limit ~post_only:true 3 Buy 1001 1 ];
  [%expect {|
    R 0 0 1 1
    R 1 0 1 2
    R 1 0 1 3
    A 1 0 1000 1
    R 1 1 1 4
    R 9 0 2 5
    A 2 1 1001 1
    R 3 0 1 7 |}]

let%expect_test "self-trade prevention: decrement and cancel (Coinbase's default)" =
  (* An 8-lot buy meets its own 5-lot offer: the offer is cancelled and the buy decremented to 3, which then
     trades with the other account's offer. This is the rule observed on real Coinbase data. *)
  run ~config:(config ~stp:Decrement_cancel ())
    [ limit ~owner:7 1 Sell 1505 5; limit ~owner:8 2 Sell 1505 5; limit ~owner:7 3 Buy 1505 8 ];
  [%expect {|
    A 1 1 1505 5
    A 2 1 1505 5
    A 3 0 1505 8
    C 1 1 1505 5 12
    M 3 0 1505 3 12
    T 3 2 0 1505 3 2 |}]

let%expect_test "self-trade prevention mode can be chosen per order" =
  run ~config:(config ~stp:Cancel_incoming ())
    [ limit ~owner:7 1 Sell 1505 5; limit ~owner:8 2 Sell 1505 5; limit ~owner:7 ~stp:Cancel_resting 3 Buy 1505 5 ];
  [%expect {|
    A 1 1 1505 5
    A 2 1 1505 5
    A 3 0 1505 5
    C 1 1 1505 5 12
    T 3 2 0 1505 5 0 |}]

let%expect_test "the book never exceeds its capacity" =
  let cfg = { (config ()) with max_orders = 2 } in
  run ~config:cfg [ limit 1 Buy 1500 1; limit 2 Buy 1501 1; limit 3 Buy 1502 1 ];
  [%expect {|
    A 1 0 1500 1
    A 2 0 1501 1
    A 3 0 1502 1
    C 3 0 1502 1 8 |}]

(* ---------------- property tests ---------------- *)

let gen_command =
  let open QCheck.Gen in
  let* kind = int_bound 9 in
  let* id = int_range 1 60 in
  let* side = map (fun b -> if b then Buy else Sell) bool in
  let* price = int_range 1480 1520 in
  let* qty = int_range 1 20 in
  let* owner = int_range 1 3 in
  let* tif = oneof_list [ Day; Day; Day; Ioc; Fok ] in
  let* stp = oneof_list [ None; None; Some Cancel_resting; Some Cancel_incoming; Some Decrement_cancel ] in
  let* post_only = map (fun n -> n = 0) (int_bound 6) in
  if kind < 6 then return (New { id; side; price; qty; ord_type = Limit; tif; post_only; stp; owner })
  else if kind = 6 then return (New { id; side; price = 0; qty; ord_type = Market; tif = Day; post_only = false; stp; owner })
  else if kind < 9 then return (Cancel_order { id })
  else return (Modify_order { id; price; qty })

let run_all cmds f =
  let _ = List.fold_left (fun book cmd -> let book, events = apply book cmd in f book cmd events; book) (create (config ())) cmds in
  ()

let prop_book_invariants =
  QCheck.Test.make ~count:500 ~name:"never crossed, no empty levels, index consistent"
    QCheck.(make Gen.(list_size (int_range 1 300) gen_command))
    (fun cmds ->
      let ok = ref true in
      run_all cmds (fun book _ _ -> if not (invariants book) then ok := false);
      !ok)

let prop_trades_respect_limits =
  QCheck.Test.make ~count:500 ~name:"no trade at a price worse than the taker's limit; no overfill"
    QCheck.(make Gen.(list_size (int_range 1 300) gen_command))
    (fun cmds ->
      let ok = ref true in
      run_all cmds (fun _ cmd events ->
          match cmd with
          | New { side; price; qty; ord_type = Limit; _ } ->
              let filled = ref 0 in
              List.iter
                (function
                  | Trade t ->
                      filled := !filled + t.qty;
                      if (side = Buy && t.price > price) || (side = Sell && t.price < price) then ok := false
                  | _ -> ())
                events;
              if !filled > qty then ok := false
          | _ -> ());
      !ok)

let%test_unit "property tests" =
  List.iter (fun t -> QCheck.Test.check_exn t) [ prop_book_invariants; prop_trades_respect_limits ]

let%test "the text encoding round-trips a configuration" =
  List.for_all
    (fun stp ->
      let c = config ~stp () in
      config_of_string (string_of_config c) = Some c)
    [ Stp_none; Cancel_resting; Cancel_incoming; Decrement_cancel ]
