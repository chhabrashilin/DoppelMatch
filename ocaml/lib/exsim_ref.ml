(* A purely functional reference model of the exchange's matching rules.

   This is a second, independent implementation of exactly the semantics of include/exsim/order_book.hpp,
   written for clarity rather than speed: an immutable book, [apply : t -> command -> t * event list], and
   algebraic data types for everything. It is differentially tested against the C++ engine (both print
   events in the canonical text format below, and the streams must be identical), and it carries its own
   expect tests and property tests. Two implementations in two languages agreeing on millions of events is
   much stronger evidence than either one's unit tests. *)

type side = Buy | Sell
type ord_type = Limit | Market
type tif = Day | Ioc | Fok
type stp = Stp_none | Cancel_resting | Cancel_incoming | Decrement_cancel

type reason =
  | No_reason
  | Invalid_order_id
  | Invalid_qty
  | Invalid_price
  | Duplicate_order_id
  | Unknown_order
  | Unknown_symbol
  | Would_cross
  | Book_full
  | User_canceled
  | Ioc_expired
  | Fok_unfilled
  | Self_trade

type request = New_order | Cancel | Modify

type command =
  | New of
      { id : int; side : side; price : int; qty : int; ord_type : ord_type; tif : tif; post_only : bool;
        stp : stp option; (* per-order override of the book's default *) owner : int }
  | Cancel_order of { id : int }
  | Modify_order of { id : int; price : int; qty : int }

type event =
  | Accepted of { id : int; side : side; price : int; qty : int }
  | Rejected of { id : int; side : side; request : request; reason : reason }
  | Trade of { taker : int; maker : int; side : side; price : int; qty : int; leaves : int }
  | Canceled of { id : int; side : side; price : int; qty : int; reason : reason }
  | Modified of { id : int; side : side; price : int; qty : int; reason : reason }

type config = { min_price : int; num_levels : int; max_orders : int; default_stp : stp }

type order = { id : int; qty : int; owner : int }

module Int_map = Map.Make (Int)

(* One side of the book: price -> FIFO queue (head = front). Best price first is handled by the caller. *)
type t = {
  config : config;
  bids : order list Int_map.t;
  asks : order list Int_map.t;
  index : (side * int) Int_map.t; (* id -> (side, price) *)
  count : int;
}

let create config = { config; bids = Int_map.empty; asks = Int_map.empty; index = Int_map.empty; count = 0 }
let opposite = function Buy -> Sell | Sell -> Buy
let book_of t = function Buy -> t.bids | Sell -> t.asks
let with_book t side b = match side with Buy -> { t with bids = b } | Sell -> { t with asks = b }
let max_price c = c.min_price + c.num_levels - 1
let in_band c px = px >= c.min_price && px <= max_price c

(* Best price of a side: highest bid, lowest ask. *)
let best t side =
  match side with
  | Buy -> Option.map fst (Int_map.max_binding_opt t.bids)
  | Sell -> Option.map fst (Int_map.min_binding_opt t.asks)

(* Does an incoming order of [side] with limit [limit] cross the opposite side? *)
let crosses t side limit =
  match best t (opposite side) with
  | None -> false
  | Some p -> ( match side with Buy -> p <= limit | Sell -> p >= limit)

(* Opposite-side levels an incoming order may trade with, in priority order. *)
let crossing_levels t side limit =
  match side with
  | Buy -> Int_map.to_seq t.asks |> Seq.take_while (fun (p, _) -> p <= limit) |> List.of_seq
  | Sell -> Int_map.to_rev_seq t.bids |> Seq.take_while (fun (p, _) -> p >= limit) |> List.of_seq

(* FOK pre-check: would the sweep resolve all of [qty] without the order being cancelled? (See fillable in
   order_book.hpp: own orders provide nothing under cancel-resting, end the sweep under cancel-incoming, and
   decrement the order under decrement-and-cancel.) *)
let fillable t side limit qty owner stp =
  let rec go need = function
    | [] -> false
    | (_, queue) :: rest ->
        let rec level need = function
          | [] -> `Continue need
          | o :: os ->
              if stp <> Stp_none && o.owner = owner then
                match stp with
                | Cancel_incoming -> `Fail
                | Decrement_cancel -> if o.qty >= need then `Fail else level (need - o.qty) os
                | _ -> level need os
              else if o.qty >= need then `Done
              else level (need - o.qty) os
        in
        (match level need queue with `Done -> true | `Fail -> false | `Continue n -> go n rest)
  in
  go qty (crossing_levels t side limit)

(* The matching loop. Returns the book, the events (in order), the unfilled quantity, and whether
   self-trade prevention stopped the incoming order. *)
let sweep t ~taker ~owner ~side ~limit ~limit_price ~qty ~stp =
  let events = ref [] in
  let emit e = events := e :: !events in
  let maker_side = opposite side in
  let rec levels t qty = function
    | [] -> (t, qty, false)
    | (px, queue) :: rest ->
        let rec walk t qty = function
          | [] -> (t, qty, [], false)
          | o :: os when stp <> Stp_none && o.owner = owner -> (
              match stp with
              | Cancel_incoming -> (t, qty, o :: os, true)
              | Decrement_cancel when o.qty > qty ->
                  let o' = { o with qty = o.qty - qty } in
                  emit (Modified { id = o.id; side = maker_side; price = px; qty = o'.qty; reason = Self_trade });
                  (t, qty, o' :: os, true)
              | _ ->
                  emit (Canceled { id = o.id; side = maker_side; price = px; qty = o.qty; reason = Self_trade });
                  let t = { t with index = Int_map.remove o.id t.index; count = t.count - 1 } in
                  if stp = Decrement_cancel then
                    if o.qty = qty then (t, qty, os, true)
                    else begin
                      let qty = qty - o.qty in
                      emit (Modified { id = taker; side; price = limit_price; qty; reason = Self_trade });
                      walk t qty os
                    end
                  else walk t qty os)
          | o :: os ->
              let fill = min o.qty qty in
              let left = o.qty - fill in
              emit (Trade { taker; maker = o.id; side; price = px; qty = fill; leaves = left });
              let qty = qty - fill in
              if left > 0 then (t, qty, { o with qty = left } :: os, false)
              else
                let t = { t with index = Int_map.remove o.id t.index; count = t.count - 1 } in
                if qty = 0 then (t, 0, os, false) else walk t qty os
        in
        let t, qty, remaining, stopped = walk t qty queue in
        let b = book_of t maker_side in
        let b = if remaining = [] then Int_map.remove px b else Int_map.add px remaining b in
        let t = with_book t maker_side b in
        if stopped || qty = 0 then (t, qty, stopped) else levels t qty rest
  in
  let t, left, stopped = levels t qty (crossing_levels t side limit) in
  (t, List.rev !events, left, stopped)

let rest t ~id ~owner ~side ~price ~qty =
  if t.count >= t.config.max_orders then (t, [ Canceled { id; side; price; qty; reason = Book_full } ])
  else
    let b = book_of t side in
    let q = Option.value (Int_map.find_opt price b) ~default:[] in
    let t = with_book t side (Int_map.add price (q @ [ { id; qty; owner } ]) b) in
    ({ t with index = Int_map.add id (side, price) t.index; count = t.count + 1 }, [])

let find t id =
  match Int_map.find_opt id t.index with
  | None -> None
  | Some (side, price) ->
      let q = Int_map.find price (book_of t side) in
      Option.map (fun o -> (side, price, o)) (List.find_opt (fun (o : order) -> o.id = id) q)

let remove t id =
  match Int_map.find_opt id t.index with
  | None -> t
  | Some (side, price) ->
      let b = book_of t side in
      let q = List.filter (fun (o : order) -> o.id <> id) (Int_map.find price b) in
      let b = if q = [] then Int_map.remove price b else Int_map.add price q b in
      { (with_book t side b) with index = Int_map.remove id t.index; count = t.count - 1 }

let set_qty t id qty =
  match Int_map.find_opt id t.index with
  | None -> t
  | Some (side, price) ->
      let b = book_of t side in
      let q = List.map (fun (o : order) -> if o.id = id then { o with qty } else o) (Int_map.find price b) in
      with_book t side (Int_map.add price q b)

let reject id side request reason = [ Rejected { id; side; request; reason } ]

let apply t cmd =
  let c = t.config in
  match cmd with
  | New { id; side; price; qty; ord_type; tif; post_only; stp; owner } ->
      let market = ord_type = Market in
      let limit = if market then (match side with Buy -> max_price c | Sell -> c.min_price) else price in
      if id = 0 then (t, reject id side New_order Invalid_order_id)
      else if qty = 0 then (t, reject id side New_order Invalid_qty)
      else if (not market) && not (in_band c price) then (t, reject id side New_order Invalid_price)
      else if Int_map.mem id t.index then (t, reject id side New_order Duplicate_order_id)
      else if post_only && crosses t side limit then (t, reject id side New_order Would_cross)
      else begin
        let px = if market then 0 else price in
        let accepted = Accepted { id; side; price = px; qty } in
        let stp = Option.value stp ~default:c.default_stp in
        if tif = Fok && not (fillable t side limit qty owner stp) then
          (t, [ accepted; Canceled { id; side; price = px; qty; reason = Fok_unfilled } ])
        else
          let t, evs, left, stopped = sweep t ~taker:id ~owner ~side ~limit ~limit_price:limit ~qty ~stp in
          let finish t more = (t, (accepted :: evs) @ more) in
          if left = 0 then finish t []
          else if stopped then finish t [ Canceled { id; side; price = px; qty = left; reason = Self_trade } ]
          else if market || tif <> Day then finish t [ Canceled { id; side; price = px; qty = left; reason = Ioc_expired } ]
          else
            let t, more = rest t ~id ~owner ~side ~price ~qty:left in
            finish t more
      end
  | Cancel_order { id } -> (
      match find t id with
      | None -> (t, reject id Buy Cancel Unknown_order)
      | Some (side, price, o) -> (remove t id, [ Canceled { id; side; price; qty = o.qty; reason = User_canceled } ]))
  | Modify_order { id; price; qty } -> (
      if qty = 0 then (t, reject id Buy Modify Invalid_qty)
      else
        match find t id with
        | None -> (t, reject id Buy Modify Unknown_order)
        | Some _ when not (in_band c price) -> (t, reject id Buy Modify Invalid_price)
        | Some (side, old_price, o) ->
            let modified = Modified { id; side; price; qty; reason = No_reason } in
            if price = old_price && qty <= o.qty then (set_qty t id qty, [ modified ])
            else
              (* cancel/replace: loses priority and may trade immediately *)
              let t = remove t id in
              let t, evs, left, stopped =
                sweep t ~taker:id ~owner:o.owner ~side ~limit:price ~limit_price:price ~qty ~stp:c.default_stp
              in
              if left = 0 then (t, (modified :: evs))
              else if stopped then (t, (modified :: evs) @ [ Canceled { id; side; price; qty = left; reason = Self_trade } ])
              else
                let t, more = rest t ~id ~owner:o.owner ~side ~price ~qty:left in
                (t, (modified :: evs) @ more))

(* ---------------- invariants (used by the property tests) ---------------- *)

let invariants t =
  let crossed =
    match (best t Buy, best t Sell) with Some b, Some a -> b >= a | _ -> false
  in
  let levels_ok b = Int_map.for_all (fun _ q -> q <> [] && List.for_all (fun (o : order) -> o.qty > 0) q) b in
  let n = Int_map.fold (fun _ q acc -> acc + List.length q) t.bids 0 + Int_map.fold (fun _ q acc -> acc + List.length q) t.asks 0 in
  (not crossed) && levels_ok t.bids && levels_ok t.asks && n = t.count && Int_map.cardinal t.index = n

(* ---------------- canonical text encoding (shared with the C++ tool exsim_difffeed) ---------------- *)

let side_code = function Buy -> 0 | Sell -> 1
let side_of = function 0 -> Buy | _ -> Sell
let stp_code = function Stp_none -> 0 | Cancel_resting -> 1 | Cancel_incoming -> 2 | Decrement_cancel -> 3
let stp_of = function 1 -> Cancel_resting | 2 -> Cancel_incoming | 3 -> Decrement_cancel | _ -> Stp_none

let reason_code = function
  | No_reason -> 0 | Invalid_order_id -> 1 | Invalid_qty -> 2 | Invalid_price -> 3 | Duplicate_order_id -> 4
  | Unknown_order -> 5 | Unknown_symbol -> 6 | Would_cross -> 7 | Book_full -> 8 | User_canceled -> 9
  | Ioc_expired -> 10 | Fok_unfilled -> 11 | Self_trade -> 12

let request_code = function New_order -> 1 | Cancel -> 2 | Modify -> 3

let string_of_event = function
  | Accepted { id; side; price; qty } -> Printf.sprintf "A %d %d %d %d" id (side_code side) price qty
  | Rejected { id; side; request; reason } ->
      Printf.sprintf "R %d %d %d %d" id (side_code side) (request_code request) (reason_code reason)
  | Trade { taker; maker; side; price; qty; leaves } ->
      Printf.sprintf "T %d %d %d %d %d %d" taker maker (side_code side) price qty leaves
  | Canceled { id; side; price; qty; reason } ->
      Printf.sprintf "C %d %d %d %d %d" id (side_code side) price qty (reason_code reason)
  | Modified { id; side; price; qty; reason } ->
      Printf.sprintf "M %d %d %d %d %d" id (side_code side) price qty (reason_code reason)

(* Commands: "N id side price qty ord_type tif flags owner" (flags as in C++: bit 0 post-only, bits 1-2 STP),
   "X id", "U id price qty". *)
let command_of_string line =
  match String.split_on_char ' ' (String.trim line) with
  | [ "N"; id; side; price; qty; ot; tif; flags; owner ] ->
      let flags = int_of_string flags in
      let stp = (flags lsr 1) land 3 in
      Some
        (New
           { id = int_of_string id; side = side_of (int_of_string side); price = int_of_string price;
             qty = int_of_string qty; ord_type = (if ot = "1" then Market else Limit);
             tif = (match tif with "1" -> Ioc | "2" -> Fok | _ -> Day); post_only = flags land 1 = 1;
             stp = (if stp = 0 then None else Some (stp_of stp)); owner = int_of_string owner })
  | [ "X"; id ] -> Some (Cancel_order { id = int_of_string id })
  | [ "U"; id; price; qty ] -> Some (Modify_order { id = int_of_string id; price = int_of_string price; qty = int_of_string qty })
  | _ -> None

let string_of_config c =
  Printf.sprintf "CONFIG %d %d %d %d" c.min_price c.num_levels c.max_orders (stp_code c.default_stp)

let config_of_string line =
  match String.split_on_char ' ' (String.trim line) with
  | [ "CONFIG"; min; levels; max_orders; stp ] ->
      Some { min_price = int_of_string min; num_levels = int_of_string levels; max_orders = int_of_string max_orders;
             default_stp = stp_of (int_of_string stp) }
  | _ -> None
